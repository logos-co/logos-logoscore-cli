# logos-logoscore-cli

`logoscore` is the headless CLI runtime for the [Logos](https://github.com/logos-co) modular application platform. It loads Logos modules (Qt plugins) and lets you call their methods from the command line — no GUI needed.

`logos-logoscore-cli` is one of two frontends for [logos-liblogos](https://github.com/logos-co/logos-liblogos):
- **logos-logoscore-cli** (this repo) — headless CLI runtime for scripting, testing, and headless deployments
- **[logos-basecamp](https://github.com/logos-co/logos-basecamp)** — the desktop GUI application shell

## How to Build

### Using Nix (Recommended)

```bash
# Build logoscore binary
nix build

# Build and run tests
nix build '.#tests'

# Build portable directory bundle
nix build '.#cli-bundle-dir'
```

The result binary is at `result/bin/logoscore`.

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

**Note:** In zsh, quote targets with `#` to prevent glob expansion:
```bash
nix build '.#logos-logoscore-cli'
```

## Usage

`logoscore` operates in two modes: **daemon mode** (long-running process with client commands) and **inline mode** (single-process, legacy).

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

The daemon writes a connection file to `~/.logoscore/daemon.json` on startup and removes it on shutdown.

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

# Get detailed module info (methods, dependencies, crash details)
logoscore module-info chat
logoscore info chat                # alias

# Call a method on a loaded module
logoscore call chat send_message "hello world"
logoscore call storage load_config @config.json   # @file reads from file

# Alternative verbose call syntax
logoscore module chat method send_message "hello"

# Watch events from a module (streams until Ctrl+C)
logoscore watch chat --event chat-message
logoscore watch chat --json        # NDJSON output

# Show resource usage for loaded modules
logoscore stats
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

For local usage, token management is automatic — the daemon writes a token to `~/.logoscore/daemon.json` and clients read it.

For remote or programmatic access:

```bash
# Via environment variable
LOGOSCORE_TOKEN=<token> logoscore list-modules --json

# Via config file
echo '{"token": "<token>"}' > ~/.logoscore/config.json
```

Token resolution order: `LOGOSCORE_TOKEN` env var → `~/.logoscore/config.json` → `~/.logoscore/daemon.json`.

#### Agent / Script Example

```bash
# Start daemon
logoscore -D -m ./modules &
sleep 2

# Preflight: verify daemon is running
logoscore status --json | jq -e '.daemon.status == "running"' > /dev/null

# Load modules
logoscore load-module chat --json

# Discover available methods
logoscore module-info chat --json | jq '.methods[].name'

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

### Inline Mode (Legacy)

Single-process mode — no daemon needed. Start the core, load modules, call methods, and exit.

```bash
logoscore -m ./modules -l waku,chat -c "chat.send_message(hello)" --quit-on-finish
```

#### Inline Options

```
  -m, --modules-dir <path>    Directory to scan for modules (repeatable)
  -l, --load-modules <modules> Comma-separated list of modules to load
  -c, --call <call>           Call a module method: module.method(arg1, arg2)
                              Use @file to read a parameter from a file.
                              Can be repeated for sequential calls.
      --quit-on-finish        Exit after all -c calls complete
```

#### Inline Examples

```bash
# Load specific modules (deps resolved automatically)
logoscore -m ./modules -l module1,module2

# Call with parameters
logoscore -l storage -c "storage.init('config', 42, true)"

# Read a parameter from a file
logoscore -l storage -c "storage.loadConfig(@config.json)"

# Multiple sequential calls (abort on first error)
logoscore -l storage \
  -c "storage.init(@config.json)" \
  -c "storage.start()"

# Multiple modules directory sources
logoscore -m ./core-modules -m ./extra-modules -l my_module
```

### Dependency Resolution

When loading modules, `logoscore` automatically resolves and loads transitive dependencies in the correct order. For example, if `logos_irc` depends on `waku_module` and `chat`, loading `logos_irc` alone is equivalent to loading `waku_module,chat,logos_irc`.

## Supported Platforms

- macOS (aarch64-darwin, x86_64-darwin)
- Linux (aarch64-linux, x86_64-linux)
