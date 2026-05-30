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
nix build 'github:logos-co/logos-logoscore-cli/5c634134d05d1f9faaf1624d3ccb67a6d72aebe6' --out-link ./logos
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

Verify the daemon is running and inspect its health:

```bash
logoscore status
```

### 3.4 List discovered modules

List modules visible in the scan directory:

```bash
logoscore list-modules
```

### 3.5 Load the module

Load `test_basic_module` into the running daemon:

```bash
logoscore load-module test_basic_module
```

### 3.6 Confirm the module is now loaded

Re-run `status`. The module that was `not_loaded` before now reports
`loaded` (the client emits JSON when its output is piped, as it is here):

```bash
logoscore status
```

### 3.7 Inspect the module with module-info

`module-info` shows a single module's status and the `Q_INVOKABLE`
methods it exposes — the same methods you can `call`:

```bash
logoscore module-info test_basic_module
```

### 3.8 Check resource usage with stats

`stats` reports per-module resource usage (process id, CPU, memory) for
the modules the daemon is running:

```bash
logoscore stats
```

### 3.9 Call methods

Invoke methods on the loaded module:

```bash
logoscore call test_basic_module returnTrue
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

### 3.10 Unload the module

Remove `test_basic_module` from the daemon:

```bash
logoscore unload-module test_basic_module
```

### 3.11 Confirm the unload took effect

A `call` against the now-unloaded module is rejected — the daemon reports
that it is not loaded:

```bash
logoscore call test_basic_module returnTrue
```

> The trailing `|| true` lets the doc-test continue past the expected
> non-zero exit code so we can assert on the error message.

### 3.12 Reload the module

`reload-module` unloads and re-loads a module in one step (handy after
rebuilding it). The module is loaded and callable again afterwards:

```bash
logoscore reload-module test_basic_module
```

```bash
logoscore call test_basic_module addInts 40 2
```

### 3.13 Stop the daemon

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

### 3.14 Confirm the daemon has stopped

A final `status` confirms the shutdown. With no daemon running, the
client reports `not_running` instead of connecting (and exits non-zero,
so we add `|| true` to let the doc-test assert on the output):

```bash
logoscore status
```
