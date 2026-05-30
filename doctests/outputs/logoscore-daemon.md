# Running Modules with the logoscore Daemon

This doc-test walks through the **daemon mode** of `logoscore`: start a long-running
process, inspect its status, load a module, call methods, and shut it down cleanly.

`logoscore` is the headless CLI frontend for the Logos platform. In daemon mode
(`-D`), it runs in the background and exposes client subcommands (`status`,
`load-module`, `call`, `stop`, …) that connect over a local RPC channel.

**What you'll learn:**

- How to build and run the logoscore CLI
- How to prepare a modules directory for the daemon to scan
- How to start the daemon in the background and capture its logs
- How to use client subcommands to inspect, load, and call modules
- How to stop the daemon when finished

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
nix build 'github:logos-co/logos-logoscore-cli/6febbc7c5d2ffcaea80cff6b28a54fc116a463e1' --out-link ./logos
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

### 3.6 Call methods

Invoke methods on the loaded module:

```bash
logoscore call test_basic_module returnTrue
```

```bash
./logos/bin/logoscore call test_basic_module addInts 2 3
```

```bash
./logos/bin/logoscore call test_basic_module echo hello
```

```bash
./logos/bin/logoscore call test_basic_module returnString
```

### 3.7 Stop the daemon

Shut down the daemon cleanly:

```bash
logoscore stop
```

The daemon removes its state file and exits. You can also stop it with
`Ctrl+C` if running in the foreground, or by sending `SIGTERM` to the
daemon PID shown in `logoscore status`.
