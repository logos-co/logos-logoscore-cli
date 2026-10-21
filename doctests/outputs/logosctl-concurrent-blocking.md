# Concurrent Clients Against a Blocking Module Method

This doc-test stress-tests the daemon's request handling with a **blocking**
module method. `test_basic_module` exposes `echoWithDelay(value, delayMs)`,
which sleeps `delayMs` milliseconds on the module's thread before returning —
so while one call is in flight, the module can do nothing else.

We start a daemon, load the module, then fire **five independent client
processes at once** (each backgrounded `logosctl call` is its own process —
the same as running the command in five separate terminals simultaneously),
every one of them invoking the 2-second blocking call. Because the module is
single-threaded, the daemon serializes the five requests: they run one after
another, so the wall-clock time is the *sum* of the sleeps (~10s), not the
longest single one (~2s).

The point of the exercise is robustness: many clients piling onto a method
that holds the module thread must **serialize cleanly — no crash, no deadlock,
no dropped replies** — and the daemon must stay healthy and responsive
afterwards.

**What you'll learn:**

- How to load a module that exposes a blocking (synchronous, sleeping) method
- How to drive one daemon from several concurrent client processes at once
- That a single-threaded module serializes concurrent calls (total time ≈ sum of the per-call sleeps), rather than running them in parallel
- That every concurrent client still receives its correct reply
- That the daemon survives the burst — no crash, all modules still loaded, and ordinary calls keep working afterwards

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

---

## Step 1: Build logosctl

Build the logosctl CLI from the published flake. The result is symlinked to
`./logos/` for easy reference in the steps below.

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli#ctl' --out-link ./logos
```

The build produces `logos/bin/logosctl` plus bundled runtime libraries and
a `logos/modules/` directory containing the built-in `capability_module`.

---

## Step 2: Prepare a modules directory

Install `test_basic_module` from logos-test-modules into a session's
`modules/` directory, `./session/modules/`, which the daemon scans along
with the bundled `capability_module` beside the binary.

### 2.1 Install a module to load

Build `test_basic_module` and copy it into the session's `modules/`
directory:

```bash
SYSTEM=$(nix eval --impure --raw --expr 'builtins.currentSystem')
nix build "github:logos-co/logos-test-modules#modules.$SYSTEM.test_basic_module.install" --out-link ./result-module
mkdir -p session/modules
cp -RL ./result-module/modules/. ./session/modules/

```

`test_basic_module` exposes `echoWithDelay(value, delayMs)` — it sleeps
`delayMs` milliseconds on the module thread, then returns `value`. That
blocking behaviour is exactly what lets us observe serialization.

---

## Step 3: Start the daemon and load the blocking module

Start the daemon in the background, then load `test_basic_module` and confirm
its blocking method works for a single call before piling on concurrent ones.

### 3.1 Start the daemon

Start logosctl in daemon mode in the background. It logs to
`session/logs/daemon.log`, and `--detach` returns only once it is
accepting commands:

```bash
logosctl --config-dir ./session daemon start --detach
```

### 3.2 Load the module

Load `test_basic_module` into the running daemon:

```bash
logosctl --config-dir ./session module load test_basic_module
```

### 3.3 Sanity-check the blocking method

A single call to `echoWithDelay` blocks for the requested time and echoes
its first argument back. Here we sleep 500 ms and get `warmup` returned:

```bash
logosctl --config-dir ./session call test_basic_module echoWithDelay warmup 500
```

---

## Step 4: Hit the blocking method from five concurrent clients

Now launch **five client processes at once**, each calling the 2-second
blocking method. Each `( logosctl call … ) &` is an independent process —
equivalent to running the command in five separate shells simultaneously.
We time the whole burst and capture each client's reply to its own file.

### 4.1 Fire five concurrent blocking calls

The five calls are submitted at the same instant. Because the module is
single-threaded, the daemon runs them one at a time, so the total
wall-clock time is the *sum* of the five 2-second sleeps (~10s) rather
than ~2s. We assert it took at least 8 seconds — proof the calls
serialized instead of running in parallel — and that nothing crashed.

```bash
# Five independent client processes, each a 2-second blocking call.
for i in 1 2 3 4 5; do
  logosctl --config-dir ./session call test_basic_module echoWithDelay "client-$i" 2000 &
done
wait
```

A single-threaded module can only run one call at a time, so five
simultaneous 2-second calls take ~10s end-to-end. If they had run
concurrently the burst would have finished in ~2s — the ≥8s wall-clock
is the observable signature of clean serialization.

### 4.2 Every client got its own correct reply

Despite contending for the same blocked module thread, each of the five
clients received exactly its own value back — no replies were dropped,
crossed, or corrupted:

```bash
cat call-1.out call-2.out call-3.out call-4.out call-5.out
```

---

## Step 5: Confirm the daemon survived the burst

The whole point is that the concurrent blocking storm causes no crash. Verify
the daemon is still running with the module loaded, that nothing crashed, and
that ordinary calls keep working.

### 5.1 Daemon and module are still healthy

The daemon is still `running`, the module still `loaded`, nothing crashed:

```bash
logosctl --config-dir ./session daemon status
```

### 5.2 Ordinary calls still work

A normal, non-blocking call succeeds after the storm:

```bash
logosctl --config-dir ./session call test_basic_module addInts 40 2
```

### 5.3 Stop the daemon

Shut down the daemon cleanly; `daemon stop` returns once it has exited:

```bash
logosctl --config-dir ./session daemon stop
```

### 5.4 Confirm the daemon has stopped

A final `status` confirms the shutdown — with no daemon running the client
reports `not_running` (and exits non-zero, so we add `|| true` to let the
doc-test assert on the output):

```bash
logosctl --config-dir ./session daemon status
```
