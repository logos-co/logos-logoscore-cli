# logos-logoscore-cli

`logosctl` is the headless CLI runtime for the [Logos](https://github.com/logos-co) modular application platform. It loads Logos modules (Qt plugins) and lets you call their methods from the command line — no GUI needed.

`logos-logoscore-cli` is one of two frontends for [logos-liblogos](https://github.com/logos-co/logos-liblogos):
- **logos-logoscore-cli** (this repo) — headless CLI runtime for scripting, testing, and headless deployments
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell

## How to Build

### Using Nix (Recommended)

`logosctl` comes in two flavors: a **dev** build for local iteration and a **portable** build for distribution. The dev build links against dev `logos-liblogos` and works with **dev** modules; the portable build is self-contained and works with **portable** modules. `nix build` (no target) produces the dev binary at `result/bin/logosctl`.

#### Dev Build

A standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it. It works with **dev** modules: those produced by a local module `nix build`, or installed by the [package manager](https://github.com/logos-co/logos-package-manager)'s dev build.

```bash
nix build                        # logosctl binary (dev) — same as '.#cli'
nix build '.#cli'
./result/bin/logosctl --help
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
./result/bin/logosctl --help
```

##### Linux AppImage (Linux only)
```bash
nix build '.#cli-appimage'
./result/logosctl.AppImage --help
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

`logosctl` runs as a **daemon** (long-running process) that you drive with **client commands** to load modules and call methods.

### Daemon Mode

Start a daemon, then use client commands to manage modules and call methods.

#### Sessions

Everything logosctl needs lives in one directory — a **session**. `--config-dir`
picks which one (default `~/.logosctl`, also `LOGOSCTL_CONFIG_DIR`):

```
<session>/
├── daemon/
│   ├── config.yaml     # daemon configuration — you write this
│   ├── daemon.log      # captured stdout/stderr when started with --detach
│   ├── state.json      # live instance: pid, instance id, bound ports
│   ├── tokens.json     # hashed-at-rest accepted tokens
│   └── tokens/<name>.json
├── client/
│   ├── config.yaml     # dial spec + token_file — you write this for remote use
│   └── auto.json       # local token, rewritten by the daemon each boot
├── modules/            # core modules installed into this session
├── plugins/            # UI plugins installed into this session
├── keyring/            # trusted package-signing keys
├── cache/downloads/    # fetched .lgx
└── data/               # per-module persistence
```

A session is **portable**: copy the directory and its packages, catalogs and
trust assumptions come with it. Two sessions can hold different package sets and
disagree about which signers they trust. The only things left outside are the
binary itself and the runtime sockets in `$TMPDIR`.

Modules come from two places: the read-only set bundled beside the binary
(`capability_module`, `package_manager`, `package_downloader`) and the writable
`<session>/modules`. On a name collision the session's copy wins, which is how
you override a bundled module.

#### Starting the Daemon

```bash
logosctl daemon start              # runs in the foreground
logosctl daemon start --detach     # forks, returns once it is accepting commands
logosctl daemon status
logosctl daemon stop
```

`--detach` is not the same as `&`: backgrounding returns immediately, before the
transports have bound, so the next command races the boot. `--detach` returns
only after the daemon has published `daemon/state.json`, and sends the daemon's
output to `daemon/daemon.log`.

#### Configuration

Configuration is a YAML document you install into the session. It is never
passed alongside another command, so `daemon start` and every client command act
on the session exactly as it is on disk.

```bash
logosctl daemon config set ./node.yaml     # also accepts @file, or - for stdin
logosctl daemon config show
```

`set` replaces the file wholesale — there is no merge — and validates before
writing, so a malformed document leaves the previous config intact. Unknown keys
are rejected by name, because the loader ignores what it does not recognise and
a silently-dropped `insecureTcp` (the key is `insecure_tcp`) would leave the
daemon running with your intent missing.

```yaml
# node.yaml
insecure_tcp: false
access_group: logos          # share the daemon with an OS group (see below)
signature_policy: warn       # none | warn | require
modules_dirs:                # extra read-only module directories to scan
  - /opt/logos/modules
modules:
  core_service:
    - protocol: tcp_ssl
      host: 0.0.0.0
      port: 8645
      codec: json            # json | cbor
      cert: tls/server.pem   # relative paths resolve inside the session
      key: tls/server.key
  capability_module:
    - protocol: tcp_ssl
      host: 0.0.0.0
      port: 8646
      cert: tls/server.pem
      key: tls/server.key
```

A `local` listener is always added to every module, so same-host clients and the
daemon's own cross-module calls keep working whatever else you configure. Any
`tcp`/`tcp_ssl` entries are additional, outward-facing listeners.

**Remote clients need `capability_module` exposed too, not just `core_service`.**
Before its first RPC a client performs a `requestModule` handshake against
`capability_module`. On the same host that rides the free local listener; from
another host it has to reach `capability_module` over the network. Exposing only
`core_service` is the single most common remote-setup mistake — client commands
hang at connect time because the handshake never completes.

Plaintext `tcp` on a non-loopback host puts tokens on the wire in cleartext. The
daemon refuses to bind that unless `insecure_tcp: true` is set. Prefer
`tcp_ssl`, or a TLS terminator in front.

The client side is symmetric:

```bash
logosctl client config set ./client.yaml
logosctl client config show
```

```yaml
# client.yaml — only needed to reach a daemon on another host
token_file: alice.json
daemon:
  core_service:      { protocol: tcp_ssl, host: node.example, port: 8645, caFile: tls/ca.pem }
  capability_module: { protocol: tcp_ssl, host: node.example, port: 8646, caFile: tls/ca.pem }
```

For same-host use you do not need this at all: the daemon writes a working
`client/config.yaml` and `client/auto.json` into the session on every boot.

> The two documents are kept separate on purpose: the daemon never reads
> `client/`, and the client never reads `daemon/`. Only the files above are
> YAML — everything the daemon and the modules own (`state.json`, `tokens.json`,
> the token files, the downloader's catalog config) stays JSON.

#### Packages

logosctl installs and manages packages itself — it bundles `package_manager` and
`package_downloader`, the same modules Basecamp uses, so both frontends drive an
identical surface. All package commands need a running daemon, because that is
where those modules live.

```bash
logosctl catalog ls                     # configured catalogs
logosctl catalog add <url>              # add one; also remove/enable/disable
logosctl catalog refresh                # re-fetch every enabled catalog

logosctl search storage                 # search the merged catalog
logosctl install storage_module --dry-run   # show what would change
logosctl install storage_module -y
logosctl package ls                     # what is installed in this session
logosctl package show storage_module    # or a path to an .lgx to inspect it
logosctl package deps storage_module -r
logosctl package upgrade storage_module -y
logosctl package remove storage_module -y
```

`install` puts files on disk; it does **not** load anything. Loading is a
separate, explicit act:

```bash
logosctl install storage_module -y
logosctl module load storage_module
```

The daemon re-scans after an install, so a freshly installed module is loadable
immediately — no restart. What `install` *does* restart is anything that was
already running and had to be stopped to make way; nothing else is touched.

Dependencies are handled by default in the direction that avoids breakage:
`install`/`upgrade` pull dependencies in (`--no-deps` opts out), and `remove`
takes dependents with it (`--no-dependents` opts out). `--dry-run` prints the
full change table plus which running modules will be stopped, and without `-y`
you are asked to confirm. With no terminal and no `-y` the operation is refused
rather than assumed — a script that forgot `--yes` should fail loudly.

Package signatures are verified against the session's own keyring:

```bash
logosctl key ls
logosctl key add acme --did did:jwk:... --display-name "Acme"
logosctl key remove acme
```

Because the keyring lives inside the session, two sessions can hold different
trust assumptions — and this is deliberately *not* shared with Basecamp's
keyring, so a key trusted in one is not automatically trusted in the other.

#### Agent / Script Example

```bash
# Start daemon
logosctl daemon start --detach &
sleep 2

# Preflight: verify daemon is running
logosctl daemon status --json | jq -e '.daemon.status == "running"' > /dev/null

# Load modules
logosctl module load chat --json

# Discover available methods (with their documentation)
logosctl module show chat --json | jq '.methods[] | {name, description}'

# Discover the events a module emits (with their documentation)
logosctl module show chat --json | jq '.events[] | {name, description}'

# Call a method
logosctl call chat send_message "hello from script" --json

# Auto-reload any crashed modules
logosctl module ls --json | jq -r '.[] | select(.status == "crashed") | .name' | while read mod; do
  logosctl module reload "$mod" --json
done

# Stream events to a log file
logosctl watch chat --event chat-message --json >> events.log &
```

#### Events Example

Modules can emit events that you can listen to in real time. Use `watch` to subscribe and `call` to trigger:

```bash
# Start daemon with a modules directory
logosctl daemon start --detach_dir &
sleep 2

# Load the module
logosctl module load test_basic_module

# Start watching for events in the background, writing to a file
logosctl watch test_basic_module --event testEvent > events.txt &
WATCH_PID=$!

# Trigger the event from another call
logosctl call test_basic_module emitTestEvent "hello world"

# Check the captured event
cat events.txt

# Clean up
kill $WATCH_PID
logosctl daemon stop
```

### Quick start: load modules and call methods

The daemon starts **clean** (it scans the module directories but loads nothing
on its own). Load modules with `load-module` — transitive dependencies are
resolved automatically — then call methods:

```bash
# Start a clean daemon scanning ./modules
logosctl daemon start --detach

# Load modules (deps resolved automatically)
logosctl module load waku
logosctl module load chat

# Call methods (positional args — see "Argument typing" below)
logosctl call chat send_message hello
logosctl call storage init config 42 true
logosctl call storage loadConfig @config.json           # @file → raw file contents
logosctl call storage setTags 'json:["a","b"]'          # json: → parsed list/map/value
logosctl call storage setLabel 'str:42'                 # str: → literal string "42"

# Multiple module directories + a custom persistence path
logosctl --config-dir /tmp/test-session daemon start --detach

# Stop the daemon when done
logosctl daemon stop
```

Daemon startup options:

```
  -D                    Start the daemon (same as `daemon start`)
  -d, --detach          Fork and return once the daemon is accepting commands
      --config-dir DIR  Which session to run
```

Everything else — module directories, persistence, transports, the access
policy — is configuration, and lives in `daemon/config.yaml`.

#### Access policy

The `access_policy` key declares, per target module, which caller modules may
invoke it:

```yaml
access_policy:
  version: 1
  mode: enforce
  restrictions:
    package_manager:    { allowedCallers: [package_manager_ui] }
    package_downloader: { allowedCallers: [package_manager_ui] }
```

The policy is handed to the runtime (via `logos_core_set_access_policy`)
before any module is loaded.

> **Note:** enforcement is not yet implemented on the runtime side — the
> policy is currently accepted and validated but **not enforced** (the
> underlying `logos_core_set_access_policy` is a no-op for now).

> **Note:** the legacy inline mode (`-c "module.method(args)"` / `--quit-on-finish`,
> which ran calls in a single short-lived process) has been removed. Use a daemon
> plus `logosctl call ...` as shown above.

### Dependency Resolution

When loading modules, `logosctl` automatically resolves and loads transitive dependencies in the correct order. For example, if `logos_irc` depends on `waku_module` and `chat`, loading `logos_irc` alone is equivalent to loading `waku_module,chat,logos_irc`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
