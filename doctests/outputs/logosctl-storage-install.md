# Installing a Package over Logos Storage with logosctl

[`logosctl-packages`](logosctl-packages.test.yaml) installs `openmetrics`
from the public catalog over HTTPS. This one installs the same package,
but it comes from the Logos Storage network.

The `package_downloader` module fetches a package over Logos Storage when
the catalog's index advertises a `logos:<network>:<cid>` URL. It needs the
`storage_module`, its optional dependency. `logosctl` ships it, and the
daemon loads it with the downloader.

When the storage module is gone, the downloader uses the package's HTTPS
URL instead.

This doc-test publishes the released `openmetrics.lgx` to a local storage
node, and serves a local catalog that points at it.

**What you'll build:** A logosctl session that installs openmetrics from a local catalog, first over Logos Storage, then over HTTPS.

**What you'll learn:**

- How the daemon brings up the storage module with the downloader
- How a catalog advertises a package over Logos Storage and over HTTPS
- How to see which URL served a download with `logosctl watch`
- How the install falls back to HTTPS when the storage module is unloaded

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

- **A Linux machine.**
- **`jq`**, **`openssl`** and **`python3`** on your `PATH`. Verify: `jq --version && openssl version && python3 --version`
- **Network access.** The released package is fetched from the public module release repository.

---

## Step 1: Build the tools

Packages in the public catalog ship **portable** variants, so this
doc-test uses the portable bundle, as `logosctl-packages` does.

### 1.1 Build the bundle

```bash
nix build 'github:logos-co/logos-logoscore-cli#ctl-bundle-dir' -o result-portable
```

### 1.2 The bundle carries the storage module

```bash
ls result-portable/modules-pkg
```

### 1.3 Build the storage node

It plays the Storage node that provides the package.

```bash
nix build 'git+https://github.com/logos-storage/logos-storage-nim?submodules=1#logos-storage-nim' -o storage-node
```

### 1.4 Build caddy

It serves the catalog and the package over HTTPS.

```bash
nix build nixpkgs#caddy -o caddy
```

### 1.5 Build the lgx tool

`index.py` uses it to read the package manifest.

```bash
nix build 'github:logos-co/logos-package#lgx' -o lgx
```

---

## Step 2: Publish openmetrics to a storage node

A release uploads packages to a Logos Storage node over its HTTP API.
The following steps reproduce that with the released `openmetrics`.

### 2.1 Download the released package

The public catalog's index gives the URL of the newest release.

```bash
curl -sSL https://github.com/logos-co/logos-modules-release/releases/download/index/index.json \
  | jq -r '.packages[] | select(.name == "openmetrics") | .versions[0].url'
curl -sSL -o openmetrics.lgx <url>
```

### 2.2 Start the Storage node

```bash
storage --data-dir=./provider-data \
  --api-port=8080 \
  --listen-port=8081 \
  --nat=extip:127.0.0.1 \
  --no-bootstrap-node &
```

### 2.3 Read its SPR

The SPR will be used as a bootstrap node for the Storage module.

```bash
curl -sS -H 'Accept: text/plain' http://127.0.0.1:8080/api/storage/v1/spr > spr.txt
```

### 2.4 Upload the package

The Storagenode answers with the package's CID.

```bash
curl --fail-with-body -sS -X POST \
  -H 'Content-Type: application/octet-stream' \
  -H 'Content-Disposition: attachment; filename="openmetrics.lgx"' \
  --data-binary @openmetrics.lgx \
  http://127.0.0.1:8080/api/storage/v1/data > cid.txt
```

---

## Step 3: Serve a catalog over local HTTPS

`logosctl catalog add` accepts `https://` URLs only, so the catalog is
served over HTTPS with a self-signed certificate.

### 3.1 Generate a certificate

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout localhost.key -out localhost.crt \
  -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1"
```

### 3.2 Start the HTTPS server

It serves the `www` directory.

```bash
caddy run --config Caddyfile --adapter caddyfile &
```

### 3.3 Build index.json

`index.json` lists the package with two URLs: the storage one first,
the HTTPS one second. `logos.dev` is the network the storage node
must be on.

```bash
echo "logos:logos.dev:$(cat cid.txt) https://127.0.0.1:8443/openmetrics.lgx ./openmetrics.lgx" > urls.txt
curl -sSLO https://raw.githubusercontent.com/logos-co/logos-modules-release-tool/main/index.py
python3 index.py build urls.txt --no-icons -o www/index.json
```

### 3.4 Write logos-repo.json

`logos-repo.json` names the catalog and points at its index.

```bash
set -eu

cat > www/logos-repo.json <<'EOF'
{
  "schemaVersion": 1,
  "name": "doctest-catalog",
  "displayName": "Doctest Catalog",
  "description": "Local catalog served over HTTPS for this doc-test.",
  "indexUrl": "https://127.0.0.1:8443/index.json",
  "trustedSigners": []
}
EOF

```

### 3.5 The HTTPS URL is dead for now

The package is not in `www`, so a successful install can only come
from the storage network.

```bash
curl -s -o /dev/null -w '%{http_code}\n' --cacert localhost.crt https://127.0.0.1:8443/openmetrics.lgx
```

---

## Step 4: Start a session

The storage module reads its node configuration from
`$HOME/.logos_storage/config.json`. The daemon runs with `HOME` set to
`./storage-home`, so it reads the file written here.

### 4.1 Write the node configuration

The provider is the only bootstrap node. `network` must match the
one in `logos:<network>:<cid>`, or the downloader ignores the CID.

```bash
mkdir -p storage-home/.logos_storage

cat > storage-home/.logos_storage/config.json <<EOF
{
    "data-dir": "$(pwd)/storage-data",
    "network": "logos.dev",
    "nat": "extip:127.0.0.1",
    "listen-port": 8082,
    "bootstrap-node": ["$(cat spr.txt)"]
}
EOF
```

### 4.2 Start the daemon

The catalog is fetched with libcurl, which must trust our
self-signed certificate.

```bash
HOME=$(pwd)/storage-home SSL_CERT_FILE=$(pwd)/localhost.crt logosctl --config-dir ./storage-session daemon start --detach
```

### 4.3 The storage module comes up with the downloader

```bash
logosctl module ls --loaded
```

### 4.4 The daemon starts the downloader

The daemon starts the downloader as soon as it loads it, as Basecamp
does.

```bash
logosctl call package_downloader getState
```

### 4.5 The downloader starts the node

The downloader sees the storage module become ready and starts its
node. Nobody starts the node by hand.

```bash
logosctl call storage_module isRunning
```

### 4.6 The node is on the catalog's network

```bash
logosctl call storage_module network
```

---

## Step 5: Find something to install

The default catalog is always present. It is removed so that the
package can only come from ours.

### 5.1 Use our catalog only

```bash
logosctl catalog remove <default url>
logosctl catalog add https://127.0.0.1:8443/logos-repo.json
logosctl catalog refresh
logosctl catalog ls
```

### 5.2 Search it

```bash
logosctl search openmetrics
```

---

## Step 6: Install over Logos Storage

### 6.1 Watch the downloader's events

`downloadDone` carries the package name (`arg0`) and the URL that
served it (`arg1`).

```bash
logosctl --json watch package_downloader > events.txt &
```

### 6.2 Preview the install

```bash
logosctl install openmetrics --dry-run
```

### 6.3 Install for real

```bash
logosctl install openmetrics -y
```

### 6.4 It came from the storage network

```bash
jq -rs 'map(select(.event == "downloadDone")) | last | .data.arg1' events.txt
```

### 6.5 The package's CID is the one served

```bash
jq -es --arg src "logos:logos.dev:$(cat cid.txt)" \
  'map(select(.event == "downloadDone")) | last | .data.arg1 == $src' events.txt

```

### 6.6 The session's node now holds the package

A node that downloads a file keeps it, and provides it to the
network.

```bash
logosctl call storage_module exists "$(cat cid.txt)"
```

---

## Step 7: Installed is not loaded

### 7.1 Discoverable, not loaded

```bash
logosctl module ls
```

### 7.2 Load it

```bash
logosctl module load openmetrics
```

### 7.3 Confirm it is running

```bash
logosctl module ls --loaded
```

---

## Step 8: Fall back to HTTPS

Without the storage module, the downloader skips the `logos:` URL and
uses the HTTPS one.

### 8.1 Remove the package

```bash
logosctl package remove openmetrics -y
```

### 8.2 Unload the storage module

```bash
logosctl module unload storage_module
```

### 8.3 Make the HTTPS URL live

```bash
cp openmetrics.lgx www/
```

### 8.4 Install again

```bash
logosctl install openmetrics -y
```

### 8.5 It came from HTTPS

```bash
jq -rs 'map(select(.event == "downloadDone")) | last | .data.arg1' events.txt
```

---

## Step 9: Shut down

### 9.1 Stop the daemon

```bash
logosctl daemon stop
```

### 9.2 Stop the watcher, the provider and the HTTPS server

```bash
kill %1 %2 %3
```
