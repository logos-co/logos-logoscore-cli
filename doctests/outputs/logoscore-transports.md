# Qt-free logoscore transport listeners

The Qt-free logoscore process uses qt_remote_plain for local, TCP, and TLS
RPC. It keeps compatibility with current qt_remote modules while Qt stays
in logos_host_qt.

**What you'll learn:**

- A local control endpoint remains available when TCP is configured
- The plain runtime binds and advertises a TCP listener

## Prerequisites

- Nix with flakes enabled.

---

## Step 1: Build logoscore

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli' --out-link ./logos
```

---

## Step 2: Bind a loopback TCP listener

Port 0 lets the OS choose a free port. The daemon records the bound port
in its runtime state and also provides a local control endpoint.

### 2.1 Start the daemon

```bash
sh -c './logos/bin/logoscore --config-dir ./session -D --module-transport core_service=tcp,host=127.0.0.1,port=0 > daemon.log 2>&1 &'
```

### 2.2 Check the bound listener and local control endpoint

```bash
for i in 1 2 3 4 5 6 7 8 9 10; do
  ./logos/bin/logoscore --config-dir ./session status --json > status.json 2>/dev/null && break
  sleep 1
done
./logos/bin/logoscore --config-dir ./session status --json
python3 - <<'PY'
import json
with open('session/daemon/state.json') as f:
    state = json.load(f)
transports = state['resolved']['modules']['core_service']['transports']
assert any(t['protocol'] == 'local' for t in transports), transports
assert any(t['protocol'] == 'tcp' and t['host'] == '127.0.0.1'
           and t['port'] > 0 for t in transports), transports
print('local control and bound TCP listener are available')
PY

```

### 2.3 Stop the daemon

```bash
./logos/bin/logoscore --config-dir ./session stop
```
