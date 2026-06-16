# Reaching the logoscore Daemon over TCP and TCP+TLS

By default the `logoscore` daemon binds every module to a local Unix socket,
so only clients on the same host — using the auto-emitted config under
`~/.logoscore/` — can reach it. This doc-test shows how to expose the daemon
over the network instead, using the two non-local transports `logoscore`
supports:

- **`tcp`** — a plaintext TCP listener. Good for loopback and trusted
  networks. On a non-loopback host it puts the auth token on the wire in
  cleartext, so the daemon refuses to bind it there without `--insecure-tcp`.
- **`tcp_ssl`** — a TLS-wrapped TCP listener. The wire is encrypted with a
  server certificate; the client verifies that certificate against a CA.

We drive everything on a single host over the loopback interface
(`127.0.0.1`) so the doc-test is self-contained and needs no second machine,
but the moving parts — a per-module network listener on the daemon, a
hand-written `client/config.json` dial spec, and a copied auth token — are
exactly the ones a real remote client uses.

> ⚠️ **Remote operation is under active development.** The transport flags,
> the `client/config.json` schema, and the auth model may change between
> releases. Local same-host use is the stable path. In particular, this
> walkthrough authenticates the network client with the daemon's
> **auto-emitted token** (`client/auto.json`), which is the credential the
> daemon registers for itself at boot — operator-issued `issue-token`
> credentials are not yet honored for `core_service` calls over the network.

**What you'll learn:**

- How the daemon's per-module transports work and why `local` is always present
- How to add a `tcp` listener to the well-known modules with `--module-transport`
- Why both `core_service` and `capability_module` must be exposed, not just one
- How the plaintext-TCP guard protects you from leaking tokens on the wire
- How to write a `client/config.json` dial spec and point a client at it with `--config-dir`
- How to copy the daemon's auth token into the client config directory
- How to drive the daemon (`status`, `load-module`, `call`, …) over TCP
- How to switch the same setup to `tcp_ssl` with a self-signed certificate
- How TLS peer verification fails closed when the client has no CA to trust

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **OpenSSL** on `PATH` (only the final TLS section needs it) to generate a
throwaway self-signed certificate.

Verify: `openssl version`

---

## Step 1: Build logoscore and prepare modules

Build the CLI and assemble a modules directory, exactly as in the daemon
doc-test. The daemon needs the bundled `capability_module` (used for the
client handshake) plus at least one ordinary module to load and call —
here `test_basic_module` from logos-test-modules.

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli/0a26e6ceebcd74a6fe1ab07c57a785dcf60b3dfe' --out-link ./logos
```

The build produces `logos/bin/logoscore` plus a `logos/modules/`
directory containing the built-in `capability_module`.

### 1.2 Install a module to load

Build `test_basic_module` and merge it with the bundled capability
module into `./modules/`:

```bash
SYSTEM=$(nix eval --impure --raw --expr 'builtins.currentSystem')
nix build "github:logos-co/logos-test-modules#modules.$SYSTEM.test_basic_module.install" --out-link ./result-module
mkdir -p modules
# cp -RL copies the read-only nix-store trees as read-only files, so a
# re-run (or a pre-populated modules/) can't overwrite them. Restore
# write permission first to keep the copy idempotent.
chmod -R u+w ./modules 2>/dev/null || true
cp -RL ./logos/modules/. ./modules/
chmod -R u+w ./modules 2>/dev/null || true
cp -RL ./result-module/modules/. ./modules/

```

```
modules/
├── capability_module/
│   ├── manifest.json
│   └── capability_module_plugin.{so,dylib}
└── test_basic_module/
    ├── manifest.json
    └── test_basic_module_plugin.{so,dylib}
```

---

## Step 2: Start the daemon with TCP listeners

`--module-transport NAME=PROTOCOL[,k=v...]` adds one network listener to
the named module. We bind **both** well-known modules on loopback:
`core_service` on port 6000 and `capability_module` on port 6001.

Two things worth calling out:

- **`local` is always present.** Every configured module keeps its
  same-host Unix-socket listener; the `tcp` entry is an *additional*
  outside-facing surface. That's what lets the daemon's own
  intra-process calls keep working while remote clients use TCP.
- **Expose `capability_module`, not just `core_service`.** Before its
  first call, a client performs a `requestModule` handshake against
  `capability_module` to resolve the endpoint. A remote client has no
  Unix socket to the daemon, so that handshake has to ride the network
  too. Expose only `core_service` and remote client commands hang or fail
  at connect time.

We use `--config-dir ./daemon-home` to keep all daemon state inside the
working directory instead of `~/.logoscore`.

### 2.1 Refuse plaintext TCP on a public interface

First, a guardrail. Plaintext `tcp` on a non-loopback host would put
the auth token on the wire in cleartext, so the daemon refuses to bind
it unless you explicitly pass `--insecure-tcp`. Watch it reject a
`0.0.0.0` listener (the `|| true` lets the doc-test continue past the
expected non-zero exit):

```bash
logoscore -D -m ./modules --module-transport core_service=tcp,host=0.0.0.0,port=6000
```

On loopback (`127.0.0.1`) there is nothing to leak to, so the same
flag binds without the escape hatch — which is what we do next.

### 2.2 Start the daemon on loopback TCP

Start the daemon in the background with TCP listeners for both
well-known modules, capturing its output to `daemon.log`:

```bash
logoscore -D -m ./modules \
    --module-transport core_service=tcp,host=127.0.0.1,port=6000 \
    --module-transport capability_module=tcp,host=127.0.0.1,port=6001 \
    > daemon.log &
```

```bash
sleep 4
```

### 2.3 Confirm the resolved transports

The daemon records the transports it actually bound in
`daemon/state.json`. Each well-known module shows the implicit
`local` listener first, followed by the `tcp` one we asked for:

```bash
cat ~/.logoscore/daemon/state.json
```

---

## Step 3: Point a client at the daemon over TCP

A network client doesn't read the daemon's files — it dials whatever its
own `client/config.json` says and authenticates with a token file beside
it. We build a **separate** config directory (`./client-home`) to make the
client/daemon split explicit, then point the client at it with
`--config-dir`.

The directory layout the client expects:

```
client-home/            ← pass this to --config-dir
└── client/
    ├── config.json     ← the dial spec
    └── auto.json       ← the auth token referenced by token_file
```

### 3.1 Copy the daemon's auth token

At boot the daemon emits its own token to
`client/auto.json` under its config dir. Copy that into the client's
`client/` directory — it's the credential the client will present:

```bash
mkdir -p ./client-home/client
cp ./daemon-home/client/auto.json ./client-home/client/auto.json

```

> On a real remote host you'd `scp` this file across instead of
> copying it locally. Treat it as a secret: anything holding it can
> drive the daemon.

### 3.2 Write the dial spec

The `client/config.json` schema is version `2`. `token_file` names the
file you just copied, and the `daemon` block lists one entry per
module with its transport, host, and port — matching the listeners the
daemon bound. Both `core_service` and `capability_module` are required:

```bash
cat > ./client-home/client/config.json <<'JSON'
{
    "version": 2,
    "token_file": "auto.json",
    "daemon": {
        "core_service":      { "transport": "tcp", "host": "127.0.0.1", "port": 6000 },
        "capability_module": { "transport": "tcp", "host": "127.0.0.1", "port": 6001 }
    }
}
JSON

```

### 3.3 Check daemon status over TCP

With the dial spec in place, every client command works against
`--config-dir ./client-home` — the traffic now goes over TCP:

```bash
logoscore --config-dir ./client-home status
```

### 3.4 List, load, and call over TCP

Drive the module lifecycle exactly as you would locally:

```bash
logoscore --config-dir ./client-home list-modules
```

```bash
logoscore --config-dir ./client-home load-module test_basic_module
```

```bash
logoscore --config-dir ./client-home call test_basic_module addInts 2 3
```

### 3.5 Stop the daemon

Shut the TCP daemon down before moving on to the TLS variant:

```bash
logoscore --config-dir ./client-home stop
```

```bash
sleep 2
```

---

## Step 4: Encrypt the wire with tcp_ssl

`tcp` is fine on loopback, but across any untrusted network you want the
wire encrypted. The `tcp_ssl` protocol wraps the same TCP listener in TLS.
The daemon side adds `cert=` and `key=` (the server's certificate and
private key); the client side adds a `ca` to verify that certificate
against, plus `verify_peer`.

TLS verifies the certificate's host name against the address the client
dialed, so the certificate must cover `127.0.0.1`. We generate a throwaway
self-signed certificate whose Subject Alternative Name includes it, and
use that same certificate as the client's CA.

### 4.1 Generate a self-signed certificate

Create a key + certificate with `subjectAltName=IP:127.0.0.1`. Because
it's self-signed, the certificate is also its own CA — so we copy it to
`ca.pem` for the client to trust:

```bash
mkdir -p tls
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout tls/key.pem -out tls/cert.pem -days 365 \
  -subj "/CN=logoscore-test" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost"
cp tls/cert.pem tls/ca.pem

```

### 4.2 Start the daemon with tcp_ssl listeners

Bind both well-known modules over `tcp_ssl`, passing the cert and key.
On loopback no `--insecure-tcp` is involved at all — TLS is never
plaintext:

```bash
logoscore -D -m ./modules \
    --module-transport "core_service=tcp_ssl,host=127.0.0.1,port=6443,cert=$PWD/tls/cert.pem,key=$PWD/tls/key.pem" \
    --module-transport "capability_module=tcp_ssl,host=127.0.0.1,port=6444,cert=$PWD/tls/cert.pem,key=$PWD/tls/key.pem" \
    > tls-daemon.log &
```

```bash
sleep 4
```

### 4.3 Write the TLS dial spec

The client config is the same shape as before, but `transport` becomes
`tcp_ssl` and each module entry gains `ca` (the certificate to trust)
and `verify_peer: true`:

```bash
mkdir -p ./tls-client-home/client
cp ./tls-daemon-home/client/auto.json ./tls-client-home/client/auto.json
cat > ./tls-client-home/client/config.json <<JSON
{
    "version": 2,
    "token_file": "auto.json",
    "daemon": {
        "core_service":      { "transport": "tcp_ssl", "host": "127.0.0.1", "port": 6443, "ca": "$PWD/tls/ca.pem", "verify_peer": true },
        "capability_module": { "transport": "tcp_ssl", "host": "127.0.0.1", "port": 6444, "ca": "$PWD/tls/ca.pem", "verify_peer": true }
    }
}
JSON

```

### 4.4 Drive the daemon over TLS

Status, load, and call now travel over an encrypted connection:

```bash
logoscore --config-dir ./tls-client-home status
```

```bash
logoscore --config-dir ./tls-client-home load-module test_basic_module
```

```bash
logoscore --config-dir ./tls-client-home call test_basic_module addInts 40 2
```

### 4.5 Verification fails closed without the CA

TLS is only as good as the verification behind it. Re-point the client
at the same `tcp_ssl` endpoint but drop the `ca` while keeping
`verify_peer: true` — the client has nothing to anchor the daemon's
self-signed certificate to, so the handshake is rejected and the
daemon is reported unreachable (the `|| true` lets the doc-test assert
on the failure):

```bash
logoscore --config-dir ./tls-client-home status   # no ca → rejected
```

> To intentionally skip verification in a dev setup, set
> `"verify_peer": false` (or pass `--no-verify-peer`) instead of
> omitting the CA — but never do that against a real network.

### 4.6 Stop the TLS daemon

```bash
logoscore --config-dir ./tls-daemon-home stop
```

```bash
sleep 2
```
