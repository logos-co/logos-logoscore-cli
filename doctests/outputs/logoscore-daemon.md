# Running Modules with the logoscore Daemon

This doc-test walks through the **daemon mode** of `logoscore`: start a long-running
process, inspect its status, load a module, introspect and call it, watch its
resource usage, unload and reload it, and finally shut the daemon down — verifying
at each stage that the command had the effect we expect.

`logoscore` is the headless CLI frontend for the Logos platform. In daemon mode
(`-D`), it runs in the background and exposes client subcommands (`status`,
`list-modules`, `load-module`, `module-info`, `stats`, `call`, `unload-module`,
`reload-module`, `stop`, …) that connect over a local RPC channel.

**What you'll learn:**

- How to build and run the logoscore CLI
- How to prepare a modules directory for the daemon to scan
- How to start the daemon in the background and capture its logs
- How to inspect the daemon and the modules it has discovered
- How to load a module, introspect it with `module-info`, and call its methods
- How to read per-module resource usage with `stats`
- How `status` reflects the module lifecycle as you load, unload, and reload
- How to stop the daemon and confirm it has actually exited

## Prerequisites

- **Nix** with flakes enabled. Install from [nixos.org](https://nixos.org/download.html), then enable flakes:

```bash
mkdir -p ~/.config/nix
echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf
```

Verify: `nix flake --help >/dev/null 2>&1 && echo "Flakes enabled"`

---

## Step 1: Build logoscore

Build the logoscore CLI from the published flake. The result is symlinked to
`./logos/` for easy reference in the steps below.

### 1.1 Build the CLI

```bash
nix build 'github:logos-co/logos-logoscore-cli/b0e2e2fb58eb42ec8f04d5352b3930c3b7e0f300' --out-link ./logos
```

The build produces `logos/bin/logoscore` plus bundled runtime libraries and
a `logos/modules/` directory containing the built-in `capability_module`.

---

## Step 2: Prepare a modules directory

`logoscore -m ./modules` scans `./modules/` for subdirectories containing
`manifest.json`. We install `test_basic_module` from logos-test-modules and
copy the bundled `capability_module` (required for auth when loading modules).

### 2.1 Install a module to load

Build `test_basic_module` and merge it with the bundled capability module
into `./modules/`:

```bash
SYSTEM=$(nix eval --impure --raw --expr 'builtins.currentSystem')
nix build "github:logos-co/logos-test-modules#modules.$SYSTEM.test_basic_module.install" --out-link ./result-module
mkdir -p modules
cp -RL ./logos/modules/. ./modules/
cp -RL ./result-module/modules/. ./modules/

```

The modules directory now contains:

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

## Step 3: Run the daemon and call a module

Start the daemon in the background, then use client subcommands to interact
with it. Output from the daemon process is captured in `logs.txt`.

### 3.1 Start the daemon

Start logoscore in daemon mode in the background, capturing output to
`logs.txt`:

```bash
logoscore -D -m ./modules > logs.txt &
```

The `-D` flag starts the daemon. The client subcommands in the following
steps connect to this running process via the config written under
`~/.logoscore/`.

```bash
sleep 3
```

### 3.2 Inspect the startup log

Review the daemon's startup output:

```bash
cat logs.txt
```

### 3.3 Check daemon status

Verify the daemon is running and inspect its health. Every client
command renders a human-readable form on a terminal and JSON when its
output is piped; `--human` and `--json` force either form explicitly:

```bash
logoscore status            # human-readable (default on a TTY)
logoscore status --json     # JSON (default when piped)
```

Human-readable form:

```text
Logoscore Daemon
  Status:       running
  PID:          12345
  Uptime:       6s
  Version:      v0.2.0

Modules: 1 loaded, 0 crashed, 1 not loaded
  test_basic_module  v1.0.0  not_loaded  -
  capability_module  v1.0.0  loaded      6s
```

JSON form:

```json
{"daemon":{"status":"running","pid":12345,"uptime_seconds":6,"version":"0.2.0"},
 "modules_summary":{"loaded":1,"crashed":0,"not_loaded":1},
 "modules":[{"name":"capability_module","status":"loaded","uptime_seconds":6,"version":"1.0.0"},
            {"name":"test_basic_module","status":"not_loaded","version":"1.0.0"}]}
```

### 3.4 List discovered modules

List modules visible in the scan directory, in both forms:

```bash
logoscore list-modules            # human-readable table
logoscore list-modules --json     # JSON
```

Human-readable table (the `NAME`/`VERSION` columns size to fit):

```text
NAME               VERSION  STATUS      UPTIME
test_basic_module  v1.0.0   not_loaded  -
capability_module  v1.0.0   loaded      6s
```

The same data as JSON — `version` is present even for `not_loaded`
modules (it comes from each module's metadata, not the running process):

```json
[{"name":"test_basic_module","status":"not_loaded","version":"1.0.0"},
 {"name":"capability_module","status":"loaded","uptime_seconds":6,"version":"1.0.0"}]
```

### 3.5 Load the module

Load `test_basic_module` into the running daemon:

```bash
logoscore load-module test_basic_module
```

### 3.6 Confirm the module is now loaded

Re-run `status`. The module that was `not_loaded` before now reports
`loaded`, and once loaded it carries an uptime (shown in both forms):

```bash
logoscore status
logoscore status --json
```

### 3.7 Module subprocesses are isolated from the launcher's process group

With a module loaded, the daemon is running it in its own subprocess
(`logos_host`). The daemon itself stays in the **foreground**, in its
launcher's process group, so a shell, `systemd`, or Docker manages it
normally — but each `logos_host` must lead its **own** process group, not
stay in the daemon's (i.e. the launcher's). Otherwise tearing the module
tree down on shutdown, or any process-group signal aimed at the daemon,
leaks into the launcher and can kill the shell driving it (a teardown
step dying with exit `-15` on Linux).

We take a module subprocess (a `logos_host` child of the daemon) and
assert it is its **own** process-group leader (`pgid == pid`) and is
**not** in the daemon's group.

```bash
# a module subprocess must lead its own group: pgid == pid, != daemon
dpid=$(logoscore status --json | jq .daemon.pid)
host=$(pgrep -P "$dpid" -f logos_host | head -1)
[ "$host" = "$(ps -o pgid= -p "$host" | tr -d ' ')" ] && echo ISOLATED
```

### 3.8 Inspect the module with module-info

`module-info` shows a single module's status and the `Q_INVOKABLE`
methods it exposes — the same methods you can `call`:

```bash
logoscore module-info test_basic_module
logoscore module-info test_basic_module --json
```

Human-readable form (methods list trimmed):

```text
Name:          test_basic_module
Version:       v1.0.0
Status:        loaded
Uptime:        6s

Methods:
  returnTrue() -> bool
  addInts(a: int, b: int) -> int
  ...
```

JSON form — version, dependencies, dependents, uptime, and the full
method/event interface in one object:

```json
{"name":"test_basic_module","version":"1.0.0","status":"loaded",
 "uptime_seconds":6,"dependencies":[],"dependents":[],
 "methods":[{"name":"returnTrue","returnType":"bool","signature":"returnTrue()"}, ...],
 "events":[]}
```

### 3.9 Check resource usage with stats

`stats` reports per-module resource usage (process id, CPU, memory) for
the modules the daemon is running:

```bash
logoscore stats
logoscore stats --json
```

Human-readable table:

```text
MODULE             PID     CPU%    MEMORY
test_basic_module  12350   0.0%    18.9 MB
capability_module  12340   4.6%    19.5 MB
```

JSON form:

```json
[{"name":"test_basic_module","cpu_percent":0.0,"cpu_time_seconds":1.02,"memory_mb":18.9},
 {"name":"capability_module","cpu_percent":4.6,"cpu_time_seconds":1.10,"memory_mb":19.5}]
```

### 3.10 Call methods

Invoke methods on the loaded module, in both forms:

```bash
logoscore call test_basic_module returnTrue
logoscore call test_basic_module returnTrue --json
```

The human-readable form prints just the return value:

```text
true
```

The JSON form wraps it with the call's metadata:

```json
{"status":"ok","module":"test_basic_module","method":"returnTrue","result":true}
```

```bash
logoscore call test_basic_module addInts 2 3
```

```bash
logoscore call test_basic_module echo hello
```

```bash
logoscore call test_basic_module returnString
```

### 3.11 Unload the module

Remove `test_basic_module` from the daemon:

```bash
logoscore unload-module test_basic_module
```

### 3.12 Confirm the unload took effect

A `call` against the now-unloaded module is rejected — the daemon reports
that it is not loaded:

```bash
logoscore call test_basic_module returnTrue
```

> The trailing `|| true` lets the doc-test continue past the expected
> non-zero exit code so we can assert on the error message.

### 3.13 Reload the module

`reload-module` unloads and re-loads a module in one step (handy after
rebuilding it). The module is loaded and callable again afterwards:

```bash
logoscore reload-module test_basic_module
```

```bash
logoscore call test_basic_module addInts 40 2
```

### 3.14 Stop the daemon

Shut down the daemon cleanly:

```bash
logoscore stop
```

The daemon removes its state file and exits. You can also stop it with
`Ctrl+C` if running in the foreground, or by sending `SIGTERM` to the
daemon PID shown in `logoscore status`.

```bash
sleep 2
```

### 3.15 Confirm the daemon has stopped

A final `status` confirms the shutdown. With no daemon running, the
client reports `not_running` instead of connecting (and exits non-zero,
so we add `|| true` to let the doc-test assert on the output):

```bash
logoscore status
```

### 3.16 A worker is not orphaned if the daemon crashes

The flip side of detaching workers into their own process group: a
module subprocess must not be left **running** if the daemon dies
*without* cleaning it up. We start a throwaway daemon, **hard-kill** it
(`SIGKILL`, so no graceful teardown runs), and confirm its module
subprocess exits on its own — via `PR_SET_PDEATHSIG` on Linux and a
`getppid` watchdog on other platforms. (Graceful shutdown, above, still
reaps workers per-PID; this covers the crash path.)

```bash
# hard-kill the daemon; its module subprocess must exit on its own
logoscore -D -m ./modules &      # (throwaway)
kill -9 "$(logoscore status --json | jq .daemon.pid)"
```
