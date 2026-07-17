# logos-logoscore-cli

`logoscore` is the headless CLI runtime for the [Logos](https://github.com/logos-co) modular application platform. It loads Logos modules (Qt plugins) and lets you call their methods from the command line — no GUI needed.

`logos-logoscore-cli` is one of two frontends for [logos-liblogos](https://github.com/logos-co/logos-liblogos):
- **logos-logoscore-cli** (this repo) — headless CLI runtime for scripting, testing, and headless deployments
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell

## How to Build

### Using Nix (Recommended)

`logoscore` comes in two flavors: a **dev** build for local iteration and a **portable** build for distribution. The dev build links against dev `logos-liblogos` and works with **dev** modules; the portable build is self-contained and works with **portable** modules. `nix build` (no target) produces the dev binary at `result/bin/logoscore`.

#### Dev Build

A standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it. It works with **dev** modules: those produced by a local module `nix build`, or installed by the [package manager](https://github.com/logos-co/logos-package-manager)'s dev build.

```bash
nix build                        # logoscore binary (dev) — same as '.#cli'
nix build '.#cli'
./result/bin/logoscore --help
```

#### Portable Builds

Portable builds are **fully self-contained** — no `/nix/store` references at runtime. They work with **portable** modules: releases from [logos-modules](https://github.com/logos-co/logos-modules), or modules installed by the package manager's portable build.

| Output | Platform | Format |
|---|---|---|
| `cli-bundle-dir` | Linux, macOS | Self-contained flat directory with `bin/`, `lib/`, and `modules/` |
| `cli-appimage` | Linux | Single-file `.AppImage` executable |

##### Self-contained directory bundle (all platforms)
```bash
nix build '.#cli-bundle-dir'
./result/bin/logoscore --help
```

##### Linux AppImage (Linux only)
```bash
nix build '.#cli-appimage'
./result/logoscore.AppImage --help
```

#### Development Shell

```bash
nix develop
```

**Note:** In zsh, quote targets containing `#` to prevent glob expansion (e.g., `'.#cli'`).

If you don't have flakes enabled globally:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

### Running Tests

```bash
# Build and run via nix
nix build '.#tests'
./result/bin/cli_tests

# Run specific tests
./result/bin/cli_tests --gtest_filter=CLITest.*

# Or via nix checks
nix flake check
```

## Usage

`logoscore` runs as a **daemon** (long-running process) that you drive with **client commands** to load modules and call methods.

### Daemon Mode

Start a daemon, then use client commands to manage modules and call methods.

#### Starting the Daemon

```bash
# Start the daemon with module directories
logoscore -D -m ./modules
logoscore daemon --modules-dir ./modules

# Start in the background
logoscore -D -m ./modules > logs.txt &
```

The daemon writes a runtime-state file (`~/.logoscore/daemon/state.json`) on startup, after transports actually bind, and removes it on clean shutdown. It also auto-emits a local-client config under `~/.logoscore/client/` (`config.json` + `auto.json`) so client commands work out of the box from the same machine.

Layout:

```
~/.logoscore/
├── daemon/
│   ├── config.json        # operator preferences (written ONLY when --persist-config is passed)
│   ├── state.json         # live-instance resolved state (created at boot, removed at shutdown)
│   ├── tokens.json        # hashed-at-rest accepted-token list (survives restarts)
│   └── tokens/
│       └── <name>.json    # raw, operator-copyable per `issue-token <name>`. 0600 perms.
└── client/
    ├── config.json        # client-owned: dial spec + token_file (write only on --persist-config)
    └── auto.json          # raw token; daemon-emitted at boot for the local client
```

Each daemon-side file has one writer and a clear lifetime:
- **`config.json`** — operator-typed preferences (transport choices, modules dirs, SSL paths). Values reflect intent: `port: 0` stays `0` (auto-pick a free port). Only written when the operator explicitly passes `--persist-config`.
- **`state.json`** — what this specific daemon process resolved (instance_id, pid, started_at, *actually-bound* port). Created at boot, deleted at shutdown.
- **`tokens.json`** — the hashed-at-rest accepted-token list. Independent of the running daemon's lifetime.

The daemon never reads `client/`; the client never reads `daemon/config.json` or `daemon/tokens.json`. (`status` consults `daemon/state.json` for a fast same-host liveness check via `kill(pid, 0)`, but never opens daemon-only secrets.) For remote clients on a different host, copy a single `daemon/tokens/<name>.json` file to the target host's `client/` dir and reference it via `token_file` in `client/config.json`.

#### `--persist-config`

CLI flags affect this run only by default. To bake them into the next launch — daemon or client side — pass `--persist-config`:

```bash
# Run the daemon with TCP listeners; this run only.
logoscore -D --module-transport core_service=tcp,port=0

# Same flags + persist them. config.json is written; next no-flag launch
# reproduces the TCP behavior. `port: 0` stays `0` in config.json (intent
# preserved); the actual bound port lives in state.json.
logoscore -D --module-transport core_service=tcp,port=0 --persist-config
```

Boot precedence is `defaults < config.json < CLI args`, with per-flag override detection: a CLI flag overrides only its own field, everything else falls through. Without `--persist-config`, no file is written.

#### Client Commands

All client commands connect to the running daemon. When stdout is not a TTY (piped or redirected), output is JSON by default. Use `--json` / `-j` to force JSON in a terminal.

```bash
# Check daemon health
logoscore status
logoscore status --json

# Load a module (dependencies resolved automatically)
logoscore load-module waku
logoscore load-module chat --json

# Unload a module
logoscore unload-module waku

# Reload a module (unload + load; falls back to load if not loaded)
logoscore reload-module chat

# List all discovered modules
logoscore list-modules
logoscore list-modules --loaded    # only loaded modules

# Get detailed module info (methods + events + descriptions, dependencies, crash details)
logoscore module-info chat
logoscore info chat                # alias

# Call a method on a loaded module
logoscore call chat send_message "hello world"
logoscore call storage load_config @config.json   # @file reads from file

# Pass a list / map / nested value with the json: prefix (see "Argument typing")
logoscore call greeter greetMany 'json:["ada","alan"]'
logoscore call config    apply    'json:{"retries":3,"debug":true}'
logoscore call greeter greet     'str:json:literal'   # str: forces a literal string

# Alternative verbose call syntax
logoscore module chat method send_message "hello"

# Watch events from a module (streams until Ctrl+C)
logoscore watch chat --event chat-message
logoscore watch chat --json        # NDJSON output

# Show resource usage for loaded modules
logoscore stats
```

#### Argument typing

Each positional argument to `call` is turned into a JSON value using the first
rule that matches, so scalars stay ergonomic while lists, maps, and literal
strings are all expressible:

| Argument form | Becomes | Example |
|---------------|---------|---------|
| `json:<value>` | the value parsed as JSON (list / map / number / any nested value) | `json:[1,2,3]`, `json:{"k":"v"}` |
| `json:@<file>` | the file's contents parsed as JSON | `json:@payload.json` |
| `str:<text>` | `<text>` verbatim as a string — no parsing, no coercion | `str:json:x` → `"json:x"`, `str:42` → `"42"` |
| `@<file>` | the file's raw contents as a string | `@config.json` |
| `true` / `false` | a boolean | `true` |
| a whole number | an integer | `42` |
| a decimal number | a double | `3.14` |
| anything else | a string | `hello` |

`json:` and `str:` are the two explicit escapes, mirroring the convention used
by `jq` (`--arg` / `--argjson`) and HTTPie (`=` / `:=`): the default path never
guesses a container, `json:` opts into parsing, and `str:` forces a literal
string for any value the default rules would otherwise reinterpret (a
number-like string, or one that itself starts with `json:` / `str:` / `@`).

**Binary (`bstr`) arguments.** JSON has no native byte type, so bytes use the
canonical tagged encoding — a JSON object `{"_bytes": "<base64url, unpadded>"}`.
Pass it like any other JSON value with `json:`:

```bash
# base64url("hello") == "aGVsbG8"
logoscore call blobstore put 'json:{"_bytes":"aGVsbG8"}'
logoscore call blobstore put 'json:@blob.json'   # {"_bytes":"..."} from a file
```

#### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | General error / daemon not running (for `status`) |
| `2` | No daemon running |
| `3` | Module error (not found, load/unload failed) |
| `4` | Method error (not found, call failed, timeout) |

#### Authentication

For local same-host use, token management is automatic. At boot the daemon
auto-issues a token named `auto` (with `local_only=true`, so it can't be used
over TCP), writes the hash into `daemon/tokens.json`, and emits the raw value
into `client/auto.json`. On the *first* boot into an empty config dir it
also writes a default `client/config.json` so local client commands work
out of the box; subsequent boots leave an existing `client/config.json`
alone (so an operator-written remote-client config isn't clobbered).

For remote or programmatic access:

```bash
# Via environment variable
LOGOSCORE_TOKEN=<token> logoscore list-modules --json
```

Token resolution order: `LOGOSCORE_TOKEN` env var → `<configDir>/client/<token_file>` (the path is whatever `client/config.json` says — defaults to `auto.json`).

##### Named client tokens

For multi-client setups (a daemon serving several remote clients, CI rotating
credentials, etc.), issue named tokens. Each entry persists as a `{name, hash,
issued_at, expires_at, local_only}` row in `daemon/tokens.json["tokens"]`; the
raw value is written to `daemon/tokens/<name>.json` (mode 0600) at issue time
so the operator can hand it off:

```bash
# Issue a token for "alice"
logoscore issue-token --name alice
logoscore issue-token --name alice --replace            # rotate
logoscore issue-token --name ci    --expires 30d        # expires after 30 days
logoscore issue-token --name probe --local-only         # only valid over LocalSocket

# List all issued tokens (names, issued_at, expires_at, local_only flag)
logoscore list-tokens

# Revoke by name — next request with that token fails auth
logoscore revoke-token alice
```

The raw value is written to `daemon/tokens/<name>.json` so the operator can hand
it off to a client host.

> **Current limitation:** named tokens are *stored* but not yet *enforced* — the
> daemon only authorizes the per-boot `auto` token today (validation against
> `daemon/tokens.json` is being wired up separately). To share a daemon with
> another OS user right now, use `--access-group` (above), which shares that
> `auto` token with the group. This note will be removed when named-token
> validation lands.

##### Plaintext-TCP guard

Plaintext `tcp` on a non-loopback host puts tokens on the wire in cleartext.
The daemon refuses to bind such a listener unless `--insecure-tcp` is
explicitly passed:

```bash
# Refused — would expose tokens.
logoscore -D --module-transport core_service=tcp,host=0.0.0.0,port=6000

# Use TLS instead.
logoscore -D \
    --module-transport core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/path/cert.pem,key=/path/key.pem

# Or, for trusted-network test setups only:
logoscore -D --module-transport core_service=tcp,host=0.0.0.0,port=6000 --insecure-tcp
```

#### Parallel Daemons (`--config-dir`)

`--config-dir <path>` overrides the default `~/.logoscore` location for the entire `daemon/` and `client/` subtree (and the module persistence tree). This lets multiple `logoscore` daemons run side-by-side against isolated state. Client commands must be invoked with the same `--config-dir` as the daemon they target.

```bash
# Two parallel daemons with isolated config/state
logoscore --config-dir /tmp/ls-a -D -m ./modules &
logoscore --config-dir /tmp/ls-b -D -m ./modules &

# Target each daemon explicitly
logoscore --config-dir /tmp/ls-a status
logoscore --config-dir /tmp/ls-b load-module waku

# Stop cleanly
logoscore --config-dir /tmp/ls-a stop
logoscore --config-dir /tmp/ls-b stop
```

Resolution order: `--config-dir` → `LOGOSCORE_CONFIG_DIR` env var → `~/.logoscore`. The flag also mirrors into `LOGOSCORE_CONFIG_DIR` so child processes inherit it.

#### Running as a system service, shared with an OS group (`--access-group`)

A common deployment runs the daemon as a dedicated service user (e.g. `logos`,
with `--config-dir /var/lib/logos-node/.logoscore`) and lets a person's own OS
account drive it. By default that fails: the daemon's local sockets are bound
owner-only (connecting to a unix socket needs *write* permission on the socket
file), and the client config/token it writes are `0600`.

`--access-group <group>` shares the daemon with an OS group:

- the module sockets are chgrp'd to the group and set to `0660`, so a member can
  connect;
- `client/config.json` and `client/auto.json` become `0640` + group-owned, and
  the config dir is made group-traversable, so a member can read the dial spec
  and the shared token (`daemon/` stays owner-only — private state is not
  shared). This is the same trust model as `docker.sock`: **group membership
  grants access.**

```bash
# As root / the service manager: create the group and add the human user.
sudo groupadd --system logos
sudo usermod -aG logos alice            # alice must re-login for this to apply

# Daemon, running as the service user `logos`:
logoscore -D -m /opt/logos/modules \
  --config-dir /var/lib/logos-node/.logoscore \
  --access-group logos

# As alice (a member of group `logos`), pointing at the service's config dir:
export LOGOSCORE_CONFIG_DIR=/var/lib/logos-node/.logoscore
logoscore status
logoscore list-modules
logoscore call my_module.some_method arg
```

The client config is regenerated on every boot, so `alice` never has to re-copy
it after a restart — the instance id changes, but the group-readable
`client/config.json` always reflects the running daemon.

> **systemd note:** do **not** set `PrivateTmp=yes` on the unit. It gives the
> service a private `/tmp` namespace, so the `/tmp/logos_*` sockets are
> *invisible* (not merely unreadable) to a client in another namespace and no
> permission change can bridge that. If you need `/tmp` isolation, that is a
> follow-up (relocating the sockets to a shared runtime dir).

#### Transports

> ⚠️ **Remote operation is very WIP and subject to change.** Everything in
> this section and the two that follow (network transports, the
> `client/config.json` dial spec, the remote client ↔ daemon walkthrough)
> is under active development. Flags, the config-file schema, and behavior
> may change without notice between releases. Local same-host use is the
> stable path; treat remote setups as experimental for now.

By default the daemon binds each well-known module (`core_service`,
`capability_module`) to a local Unix socket only — clients must run on the
same host. To reach the daemon from another machine, a container, or
across NAT, configure a network listener per module via `--module-transport`:

```
--module-transport NAME=PROTOCOL[,k=v[,k=v...]]
```

`NAME` is any module the daemon will load; `PROTOCOL` is `local`, `tcp`,
or `tcp_ssl`. The optional `k=v` pairs configure the protocol: `host`,
`port`, `codec` (`json` default | `cbor`), and
`ca` / `cert` / `key` / `verify_peer` for `tcp_ssl`. The flag is
repeatable — each appearance adds one more listener to the named module.

**Local is always present.** Every module the operator configures (and
the two well-known ones — `core_service` and `capability_module`)
automatically gets a `local` listener prepended to whatever the
operator named. The TCP / TCP+SSL flags add *additional*
outside-facing listeners; they don't replace the same-host one.

This means the examples below — and any `--module-transport NAME=tcp,...`
invocation generally — don't need a separate `--module-transport
NAME=local` line. The same-host LocalSocket listener is bound for free,
which lets every **intra-daemon** code path (cross-module outbound
`getClient(name)` calls, the parent's `notifyCapabilityModule`
handshake) keep working over LocalSocket while remote clients use the
operator-configured TCP endpoint.

> **Remote clients need `capability_module` exposed too — not just
> `core_service`.** A client command doesn't talk only to `core_service`.
> Before its first RPC, the client's own `LogosAPIClient` performs a
> `requestModule` handshake against **`capability_module`** to resolve the
> endpoint. On the same host that handshake rides the free LocalSocket
> listener, so a local client only ever needs `core_service`. A client on
> *another* host has no LocalSocket to the daemon — its `requestModule`
> call has to reach `capability_module` over the network. So a remote
> daemon must add a TCP (or `tcp_ssl`) listener to **both** well-known
> modules:
>
> ```bash
> logoscore -D -m ./modules \
>     --module-transport core_service=tcp,host=0.0.0.0,port=8645 \
>     --module-transport capability_module=tcp,host=0.0.0.0,port=8646 \
>     --insecure-tcp
> ```
>
> Expose only `core_service` and client commands hang or fail at connect
> time, because the `capability_module` handshake never completes. This is
> the single most common remote-setup mistake.

```bash
# TCP — plaintext, good for localhost or trusted networks. Local
# listeners are added automatically; just name the TCP one.
logoscore -D -m ./modules \
    --module-transport core_service=tcp,host=127.0.0.1,port=6000 \
    --module-transport capability_module=tcp,host=127.0.0.1,port=6001

# TCP + TLS — wire-encrypted; cert + key required, CA optional. Local
# listeners are still bound implicitly for same-host clients.
logoscore -D -m ./modules \
    --module-transport "core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem,ca=/etc/logoscore/ca.pem" \
    --module-transport "capability_module=tcp_ssl,host=0.0.0.0,port=6444,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem,ca=/etc/logoscore/ca.pem"

# Defaults: omit --module-transport entirely and the well-known modules
# get a single `local` listener each. Most local-development setups just
# want this.
logoscore -D -m ./modules

# Per-module: applies to user modules too. The operator's TCP listener
# is the additional surface; LocalSocket is always there for in-process
# / on-host callers.
logoscore -D -m ./modules \
    --module-transport my_module=tcp,host=127.0.0.1,port=6010
```

##### Client-side dial spec

The client never reads daemon-only files (`daemon/config.json`,
`daemon/tokens.json`) — it dials whatever `<configDir>/client/config.json`
says. The daemon auto-emits one for the local same-host case at boot
(LocalSocket pointing at the daemon's freshly-issued `auto.json` token).
For remote clients (docker `-p` port-forwarding, NAT, SSH tunnels) you
write `client/config.json` yourself.

**`client/config.json` schema** (`version` must be `2`):

```jsonc
{
    "version": 2,
    "token_file": "bob.json",      // filename inside the SAME client/ dir
    "instance_id": "a3f1c8d20b4e",   // OPTIONAL — only the local-socket dial
                                     // path needs it; omit for remote TCP
    "daemon": {
        "core_service": {
            "transport": "tcp",      // "local" | "tcp" | "tcp_ssl"
            "host":      "192.168.1.20",
            "port":      8645,
            "codec":     "json"      // OPTIONAL — "json" (default) | "cbor"
        },
        "capability_module": {       // REQUIRED for remote clients (see above)
            "transport": "tcp",
            "host":      "192.168.1.20",
            "port":      8646
        }
    }
}
```

Notes on the schema:
- The per-module key is `"transport"`, **not** `"protocol"`. The reader
  uses a strict allowlist: a typo (e.g. `"tcp_ssll"`) fails the whole parse
  with a clear error rather than silently dialing the wrong endpoint.
- `daemon.core_service` is mandatory; `daemon.capability_module` is required
  in practice for any remote (non-LocalSocket) client — see the callout in
  **Transports** above.
- The two entries can point at **different ports** — they're independent
  listeners on the daemon (e.g. `8645` and `8646` above).
- For `tcp_ssl`, add `"ca": "/path/ca.pem"` and `"verify_peer": true|false`.
- `host`/`port` are the **dial** address. Behind docker `-p`, NAT, or an SSH
  tunnel this is the reachable address, which may differ from the `0.0.0.0`
  the daemon bound.

**Where the file lives — `--config-dir`.** `client/config.json` and the
token file both live in a `client/` subdirectory. `--config-dir` points at
the directory that *contains* `client/`, not at `client/` itself:

```
my-client-dir/            ← pass this to --config-dir
└── client/
    ├── config.json       ← the dial spec above
    └── bob.json        ← the raw token file referenced by token_file
```

```bash
logoscore --config-dir ./my-client-dir status
logoscore --config-dir ./my-client-dir list-modules
```

(When unset, `--config-dir` defaults to `~/.logoscore`, so on the daemon's
own host the auto-emitted `~/.logoscore/client/` tree is used with no flag.)

##### End-to-end: remote client ↔ daemon

A complete walkthrough for a client on one host talking to a daemon on
another (`192.168.1.20`). Mirrors a real plaintext-TCP setup on a trusted
LAN; for anything crossing an untrusted network use `tcp_ssl` (final step).

**1. On the daemon host** — expose both well-known modules over TCP and
mint a token for this client:

```bash
# Bind core_service + capability_module on all interfaces. --insecure-tcp
# is required because plaintext tcp on a non-loopback host puts the token
# on the wire in cleartext (use tcp_ssl to avoid the flag — see below).
logoscore daemon --modules-dir /home/me/logos/modules/ \
    --module-transport core_service=tcp,host=0.0.0.0,port=8645 \
    --module-transport capability_module=tcp,host=0.0.0.0,port=8646 \
    --insecure-tcp &

# Mint a named token for the remote client. Writes the raw value to
# ~/.logoscore/daemon/tokens/bob.json (mode 0600).
logoscore issue-token --name bob
```

**2. Move the token to the client host.** Copy the raw token file across
(scp / ansible / your secret store) into the client's `client/` dir:

```bash
# Run on, or targeting, the client host:
mkdir -p ./my-client-dir/client
scp daemon-host:~/.logoscore/daemon/tokens/bob.json ./my-client-dir/client/
```

After copying, the daemon-side `daemon/tokens/bob.json` may be deleted —
the daemon validates against the stored hash, not the raw file.

**3. On the client host** — write `./my-client-dir/client/config.json`
pointing at the daemon's IP and the two ports, with `token_file` naming the
file you just copied:

```json
{
    "version": 2,
    "token_file": "bob.json",
    "daemon": {
        "core_service":      { "transport": "tcp", "host": "192.168.1.20", "port": 8645 },
        "capability_module": { "transport": "tcp", "host": "192.168.1.20", "port": 8646 }
    }
}
```

**4. Run client commands** with `--config-dir` pointing at the directory
that contains `client/`:

```bash
logoscore --config-dir ./my-client-dir status
logoscore --config-dir ./my-client-dir list-modules
logoscore --config-dir ./my-client-dir load-module accounts_module
logoscore --config-dir ./my-client-dir module-info accounts_module
logoscore --config-dir ./my-client-dir call accounts_module createRandomMnemonicWithDefaultLength
```

**TLS variant.** To drop `--insecure-tcp` and encrypt the wire, bind
`tcp_ssl` on the daemon and point the client at the CA:

```bash
# Daemon
logoscore daemon -m /home/me/logos/modules/ \
    --module-transport "core_service=tcp_ssl,host=0.0.0.0,port=8645,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem" \
    --module-transport "capability_module=tcp_ssl,host=0.0.0.0,port=8646,cert=/etc/logoscore/cert.pem,key=/etc/logoscore/key.pem"
```

```json
// client/config.json — transport becomes tcp_ssl + ca/verify_peer
{
    "version": 2,
    "token_file": "bob.json",
    "daemon": {
        "core_service":      { "transport": "tcp_ssl", "host": "192.168.1.20", "port": 8645, "ca": "/etc/logoscore/ca.pem", "verify_peer": true },
        "capability_module": { "transport": "tcp_ssl", "host": "192.168.1.20", "port": 8646, "ca": "/etc/logoscore/ca.pem", "verify_peer": true }
    }
}
```

#### Agent / Script Example

```bash
# Start daemon
logoscore -D -m ./modules &
sleep 2

# Preflight: verify daemon is running
logoscore status --json | jq -e '.daemon.status == "running"' > /dev/null

# Load modules
logoscore load-module chat --json

# Discover available methods (with their documentation)
logoscore module-info chat --json | jq '.methods[] | {name, description}'

# Discover the events a module emits (with their documentation)
logoscore module-info chat --json | jq '.events[] | {name, description}'

# Call a method
logoscore call chat send_message "hello from script" --json

# Auto-reload any crashed modules
logoscore list-modules --json | jq -r '.[] | select(.status == "crashed") | .name' | while read mod; do
  logoscore reload-module "$mod" --json
done

# Stream events to a log file
logoscore watch chat --event chat-message --json >> events.log &
```

#### Events Example

Modules can emit events that you can listen to in real time. Use `watch` to subscribe and `call` to trigger:

```bash
# Start daemon with a modules directory
logoscore -D -m ./modules_dir &
sleep 2

# Load the module
logoscore load-module test_basic_module

# Start watching for events in the background, writing to a file
logoscore watch test_basic_module --event testEvent > events.txt &
WATCH_PID=$!

# Trigger the event from another call
logoscore call test_basic_module emitTestEvent "hello world"

# Check the captured event
cat events.txt

# Clean up
kill $WATCH_PID
logoscore stop
```

### Quick start: load modules and call methods

The daemon starts **clean** (it scans the module directories but loads nothing
on its own). Load modules with `load-module` — transitive dependencies are
resolved automatically — then call methods:

```bash
# Start a clean daemon scanning ./modules
logoscore -D -m ./modules

# Load modules (deps resolved automatically)
logoscore load-module waku
logoscore load-module chat

# Call methods (positional args — see "Argument typing" below)
logoscore call chat send_message hello
logoscore call storage init config 42 true
logoscore call storage loadConfig @config.json           # @file → raw file contents
logoscore call storage setTags 'json:["a","b"]'          # json: → parsed list/map/value
logoscore call storage setLabel 'str:42'                 # str: → literal string "42"

# Multiple module directories + a custom persistence path
logoscore -D -m ./core-modules -m ./extra-modules --persistence-path /tmp/test-data

# Stop the daemon when done
logoscore stop
```

Daemon startup options:

```
  -D                             Start the daemon
  -m, --modules-dir <path>       Directory to scan for modules (repeatable)
      --persistence-path <path>  Base directory for module instance persistence
                                 (default: ~/.logoscore/data)
      --access-policy <arg>      Inter-module access policy: a path to a JSON
                                 file, or inline JSON (mode + per-target caller
                                 allowlists). See "Access policy" below.
```

#### Access policy

`--access-policy` installs an inter-module access policy that declares,
per target module, which caller modules are allowed to invoke it. The
argument is resolved as **a path to a JSON file** when it doesn't begin
with `{`, or as **inline JSON** when it does. The resolved document is
validated as parseable JSON before the daemon boots; a bad path or
malformed JSON aborts startup.

```json
{
  "version": 1,
  "mode": "enforce",
  "restrictions": {
    "package_manager":    { "allowedCallers": ["package_manager_ui"] },
    "package_downloader": { "allowedCallers": ["package_manager_ui"] }
  }
}
```

```bash
# From a file
logoscore -D -m ./modules --access-policy ./policy.json

# Inline
logoscore -D -m ./modules \
  --access-policy '{"version":1,"mode":"enforce","restrictions":{"package_manager":{"allowedCallers":["package_manager_ui"]}}}'
```

The policy is handed to the runtime (via `logos_core_set_access_policy`)
before any module is loaded, and is persisted with `--persist-config`
like the other daemon flags.

> **Note:** enforcement is not yet implemented on the runtime side — the
> policy is currently accepted and validated but **not enforced** (the
> underlying `logos_core_set_access_policy` is a no-op for now).

> **Note:** the legacy inline mode (`-c "module.method(args)"` / `--quit-on-finish`,
> which ran calls in a single short-lived process) has been removed. Use a daemon
> plus `logoscore call ...` as shown above.

### Dependency Resolution

When loading modules, `logoscore` automatically resolves and loads transitive dependencies in the correct order. For example, if `logos_irc` depends on `waku_module` and `chat`, loading `logos_irc` alone is equivalent to loading `waku_module,chat,logos_irc`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
