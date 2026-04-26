# Logoscore CLI — Project Description

## Overview

The `logoscore` CLI is a standalone application that provides the command-line interface for the Logos Core runtime. It depends on **liblogos**, a C library that provides the core runtime (plugin discovery, loading, dependency resolution, event loop). The CLI is responsible for:

- Running as a daemon that hosts the liblogos runtime
- Providing client commands that talk to the daemon via RPC
- Supporting inline mode for quick one-off operations

This project will live in its own repository. liblogos is consumed as an external C library dependency.

## Project Structure

```
logoscore-cli/
├── src/                              # All CLI source code
│   ├── main.cpp                      # Entry point — detects mode, dispatches
│   ├── config.cpp/h                  # Token + config file resolution
│   │
│   ├── daemon/                       # Daemon path (logoscore -D)
│   │   ├── daemon.cpp/h              # Start core, load core_service, run event loop,
│   │   │                             # open each --transport listener
│   │   ├── connection_file.cpp/h     # Write/read/cleanup <configDir>/daemon.json;
│   │   │                             # carries the `transports` array
│   │   └── token_store.cpp/h         # Named-token DB (<configDir>/tokens.db +
│   │                                 # per-client tokens/<name>.json)
│   │
│   ├── client/                       # Client path (all subcommands)
│   │   ├── client.cpp/h              # Connect to daemon's core_service via LogosAPIClient
│   │   ├── client_connection.cpp/h   # Client-side transport resolver: picks one of the
│   │   │                             # advertised transports, applies --client-tcp-host/
│   │   │                             # --client-tcp-port/--no-verify-peer overrides
│   │   ├── output.cpp/h              # Output formatter (human / JSON / NDJSON)
│   │   └── commands/                 # Subcommand implementations
│   │       ├── command.cpp/h         # Base command class
│   │       ├── status_command.cpp/h
│   │       ├── load_module_command.cpp/h
│   │       ├── unload_module_command.cpp/h
│   │       ├── reload_module_command.cpp/h
│   │       ├── list_modules_command.cpp/h
│   │       ├── module_info_command.cpp/h
│   │       ├── call_command.cpp/h
│   │       ├── watch_command.cpp/h
│   │       ├── stats_command.cpp/h
│   │       ├── stop_command.cpp/h
│   │       ├── issue_token_command.cpp/h    # Mints named tokens (tokens/<name>.json)
│   │       ├── revoke_token_command.cpp/h   # Revokes by name
│   │       └── list_tokens_command.cpp/h    # Lists issued tokens (hashes, not plaintext)
│   │
│   ├── inline/                       # Inline path (-m, -l, -c flags)
│   │   ├── command_line_parser.cpp/h  # Argument parser for flat-flag interface
│   │   └── call_executor.cpp/h        # Direct method execution via C API
│   │
│   └── core_service/                 # Built-in module — CLI ↔ daemon RPC gateway
│       ├── core_service_impl.h       # LOGOS_PROVIDER class with LOGOS_METHOD declarations
│       ├── core_service_impl.cpp     # Method implementations (delegates to liblogos C API)
│       ├── core_service_loader.h     # LogosProviderPlugin loader
│       ├── metadata.json             # Plugin metadata
│       └── core_service_dispatch.cpp    # Manual callMethod/getMethods dispatch
│
├── tests/
│   ├── test_core_service.cpp         # core_service method tests
│   ├── test_cli_commands.cpp         # Mode detection, subcommand dispatch
│   ├── test_cli_output.cpp           # Output formatter tests
│   ├── test_cli_daemon.cpp           # Daemon lifecycle, connection file
│   ├── test_cli_client.cpp           # Client connection to core_service
│   └── test_inline.cpp              # Inline argument parsing tests
│
├── docs/
│   ├── spec.md                       # CLI specification (user-facing behavior)
│   └── project.md                    # This file (implementation details)
│
├── CMakeLists.txt                    # Build configuration
├── flake.nix                         # Nix flake
└── nix/                              # Nix build modules
```

## Dependencies

| Dependency | Type | Purpose |
|---|---|---|
| **liblogos** | C library (external) | Core runtime: plugin discovery, loading, dependency resolution, event loop, process stats |
| **logos-cpp-sdk** | C++ library (external) | RPC client/provider classes: LogosAPI, LogosAPIClient, LogosProviderBase, TokenManager |
| **logos-cpp-generator** | Build tool (external) | Code generator for LOGOS_METHOD dispatch tables |
| **Qt6 Core** | Framework | Event loop, JSON handling, process management |
| **Qt6 RemoteObjects** | Framework | IPC between daemon and module host processes |
| **CMake 3.14+** | Build system | — |
| **Google Test** | Test framework | — |
| **Nix** | Package manager | Reproducible builds |

### liblogos C API surface used

The CLI uses these functions from liblogos (declared in `logos_core.h`):

| Function | Used by |
|---|---|
| `logos_core_init(argc, argv)` | Daemon, Inline |
| `logos_core_add_plugins_dir(path)` | Daemon, Inline |
| `logos_core_start()` | Daemon, Inline |
| `logos_core_exec()` | Daemon, Inline |
| `logos_core_cleanup()` | Daemon |
| `logos_core_load_plugin_with_dependencies(name)` | core_service, Inline |
| `logos_core_unload_plugin(name)` | core_service |
| `logos_core_get_known_plugins()` | core_service |
| `logos_core_get_loaded_plugins()` | core_service |
| `logos_core_get_module_stats()` | core_service |

---

## CLI Execution Paths

The `logoscore` binary detects its mode from the first argument and dispatches to one of three paths:

```
logoscore -D / daemon         →  Daemon path    (long-running, hosts modules)
logoscore <subcommand>        →  Client path    (short-lived, talks to daemon)
logoscore -m -l -c            →  Inline path    (single-process, no daemon needed)
```

### Detection logic (main.cpp)

Before mode detection, `main()` scans argv for `-v`/`--verbose` and installs a custom Qt message handler that suppresses debug/info/warning logs unless verbose is set.

```
if argv contains "-D" or "daemon"      → daemon path
else if argv[1] is a known subcommand  → client path
else if argv contains -m, -l, or -c    → inline path
else                                   → print help
```

### Daemon Path (`logoscore -D`)

```
main.cpp
  → Daemon::start(modulesDirs, persistencePath, transportInfos)
    1. Generate instance ID, set LOGOS_INSTANCE_ID env var
    2. logos_core_init(argc, argv)
    3. logos_core_add_plugins_dir() for each -m path
    4. logos_core_start()                         // discover plugins
    5. Register core_service in-process via LogosAPI/LogosAPIProvider.
       If --transport=tcp / --transport=tcp_ssl flags were passed, pass the
       resolved TransportInfos to LogosAPIProvider so it opens one listener
       per transport simultaneously (local + tcp + tcp_ssl).
    6. Save client token via TokenManager
    7. Write <configDir>/daemon.json              // connection file, with
                                                  // `transports` array so
                                                  // clients can pick one
    8. Print startup message to stdout
    9. logos_core_exec()                          // Qt event loop (blocks)
   10. On SIGINT/SIGTERM or shutdown RPC:
       logos_core_cleanup()
       Remove connection file
       exit(0)
```

The daemon path calls the liblogos C API directly. It owns the runtime and hosts all modules, including the built-in `core_service` module. Startup/shutdown messages go to stdout (so `> logs.txt` works); debug logs go to stderr and are suppressed unless `--verbose` is passed.

**Multi-transport.** `--transport` is repeatable. When the daemon exposes
`core_service` over several transports at once, each one becomes an entry in
`daemon.json`'s `transports` array and the provider maintains one listener per
entry. Module ↔ module traffic inside the process group stays on the local
socket regardless; only `core_service`'s external surface is affected.

**Named tokens.** `TokenStore` (owned by the daemon) persists the issued-token
map in `<configDir>/tokens.db`. The automatically-generated CLI client token
is written into `daemon.json` the same way as before. Named tokens from
`issue-token --name <n>` are additionally written to `tokens/<n>.json` for
distribution to a specific client.

### Client Path (`logoscore <subcommand>`)

```
main.cpp
  → Client::connect()
    1. Read <configDir>/daemon.json (instance_id + token + `transports` array)
    2. Set LOGOS_INSTANCE_ID env var from instance_id
       → now LogosInstance::id("core_service") returns the correct registry URL
    3. Pick a transport:
       - $LOGOSCORE_CLIENT_TRANSPORT (from --client-transport) if advertised
       - else "local" if advertised
       - else error (no viable transport)
    4. ClientConnection::effectiveTransport(chosen) applies env overrides
       (LOGOSCORE_CLIENT_TCP_HOST / _TCP_PORT / _NO_VERIFY_PEER) so the
       dialed endpoint can differ from the advertised one (NAT, port-forward,
       SSH tunnel)
    5. Validate codec: fail fast if --client-codec disagrees with the codec
       the daemon advertised for the chosen transport (prevents mixed-codec
       corruption)
    6. Keep the resolved LogosTransportConfig for this connection attempt
       (do *not* install it as a process-wide default — the SDK's
       LogosAPIProvider reads the global default to bind its own server
       socket, so flipping the default to e.g. tcp_ssl on the client side
       would try to bind a TLS server with no cert/key and abort)
    7. Create LogosAPIClient targeting "core_service" with that explicit
       transport config (LogosAPI itself stays on the local-socket default)
    8. Authenticate with token
  → Command::execute(args)
    1. Call LOGOS_METHOD on core_service via LogosAPIClient
    2. Format result (human / JSON)
    3. Print to stdout, exit
```

Client commands **never** call liblogos C API functions. They talk exclusively to the daemon's `core_service` module via the SDK's RPC mechanism. This means the client path depends only on `logos-cpp-sdk`, not on `liblogos`.

**Liveness** is no longer a separate pre-check. The previous PID-alive probe
only worked for local daemons — it's meaningless for a daemon in a container
or across NAT. The first RPC (commonly `status`) surfaces connect failures
through the same timeout/error path as any other method, so there's one error
story. `ConnectionFile::read().fileOk` now just reflects "file exists and
parses" — the on-disk precondition, not liveness.

### Inline Path (`logoscore -m -l -c`)

```
main.cpp
  → parseCommandLineArgs(app)                 // existing parser
  → logos_core_init / start / exec            // starts core in same process
  → CallExecutor::executeCalls(calls)         // direct C API calls
  → exit or event loop
```

The inline path starts the core in the same process, loads modules, executes calls, and exits. No daemon, no core_service. This is the existing behavior and remains unchanged.

---

## CoreService Module

The `core_service` module is the RPC gateway between CLI clients and the daemon. It is a proper Logos module built with the new SDK API (`LOGOS_PROVIDER`, `LOGOS_METHOD`), but it lives in the CLI codebase (not in liblogos) because it is the CLI's concern — it exists to serve CLI clients.

### Why a module?

- Uses the same SDK API as any other module — no special plumbing
- CLI clients connect to it via `LogosAPIClient`, same as module-to-module communication
- Auth tokens work the same way (TokenManager validates the client token)
- Events can be forwarded using the standard event system
- If needed in the future, it could be extracted into a standalone plugin

### Definition

**Files:** `src/core_service/core_service_impl.h`

```cpp
#include <logos_provider_object.h>

class CoreServiceImpl : public LogosProviderBase
{
    LOGOS_PROVIDER(CoreServiceImpl, "core_service", "1.0.0")

public:
    // Module lifecycle
    LOGOS_METHOD QVariant loadModule(const QString& name);
    LOGOS_METHOD QVariant unloadModule(const QString& name);
    LOGOS_METHOD QVariant reloadModule(const QString& name);

    // Queries
    LOGOS_METHOD QJsonArray listModules(const QString& filter);
    LOGOS_METHOD QJsonObject getStatus();
    LOGOS_METHOD QJsonObject getModuleInfo(const QString& name);
    LOGOS_METHOD QJsonArray  getModuleStats();

    // Proxied call — delegates to target module
    LOGOS_METHOD QVariant callModuleMethod(const QString& module,
                                          const QString& method,
                                          const QVariantList& args);

    // Event forwarding
    LOGOS_METHOD bool watchModuleEvents(const QString& module,
                                       const QString& eventName);

    // Daemon lifecycle
    LOGOS_METHOD QJsonObject shutdown();

protected:
    void onInit(LogosAPI* api) override;

private:
    LogosAPI* m_api = nullptr;
};
```

### How each method works

| LOGOS_METHOD | What it does (daemon-side) |
|---|---|
| `loadModule(name)` | Calls `logos_core_load_plugin_with_dependencies(name)`. Returns `{"status":"ok","module":"...","version":"...","dependencies_loaded":[...]}` |
| `unloadModule(name)` | Calls `logos_core_unload_plugin(name)`. Returns `{"status":"ok","module":"..."}` |
| `reloadModule(name)` | Checks if loaded/crashed → unload if needed → load. Returns result with `previous_status` |
| `listModules(filter)` | Calls `logos_core_get_known_plugins()` + `logos_core_get_loaded_plugins()`. Merges with crash metadata. Returns JSON array with status enum |
| `getStatus()` | Reads daemon state (PID, uptime, version) + calls `listModules("all")`. Returns `{"daemon":{...},"modules_summary":{...},"modules":[...]}` |
| `getModuleInfo(name)` | Fetches metadata, methods (via SDK introspection), process info, crash history. Returns extended JSON |
| `getModuleStats()` | Calls `logos_core_get_module_stats()`. Returns CPU/memory per module |
| `callModuleMethod(module, method, args)` | Uses `m_api->getClient(module)->invokeRemoteMethod()` to proxy the call to the target module. Returns the result. `LogosResult` return values are unpacked into `{success, value, error}` here so that the JSON shape is identical regardless of whether the daemon-module hop went over the local socket (QRO) or the plain-C++ transport (tcp / tcp_ssl). |
| `watchModuleEvents(module, event)` | Registers an event listener on the target module via `m_api->getClient(module)->onEvent()`. Forwards received events by calling `emitEvent()` on core_service, which the CLI client receives over its own event subscription |
| `shutdown()` | Schedules `QCoreApplication::quit()` after a 200ms delay (to allow the RPC response to be sent), then the daemon performs its normal cleanup (unload modules, remove connection file, exit) |

### Loader

**Files:** `src/core_service/core_service_loader.h`

```cpp
class CoreServiceLoader : public QObject, public PluginInterface, public LogosProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LogosProviderPlugin_iid FILE "metadata.json")
    Q_INTERFACES(PluginInterface LogosProviderPlugin)

public:
    QString name() const override { return "core_service"; }
    QString version() const override { return "1.0.0"; }
    LogosProviderObject* createProviderObject() override {
        return new CoreServiceImpl();
    }
};
```

### Metadata

**Files:** `src/core_service/metadata.json`

```json
{
  "name": "core_service",
  "version": "1.0.0",
  "type": "core",
  "category": "management",
  "description": "RPC gateway for CLI client commands"
}
```

### Registration (daemon-side)

The daemon registers `core_service` as an in-process module during startup, before entering the event loop:

```cpp
// In Daemon::start()
auto* coreServiceApi = new LogosAPI("core_service");
auto* coreServiceImpl = new CoreServiceImpl();
coreServiceImpl->init(coreServiceApi);

auto* provider = coreServiceApi->getProvider();
provider->registerObject("core_service", static_cast<LogosProviderObject*>(coreServiceImpl));
```

This registers the module directly into the runtime using the C++ SDK classes (`LogosAPI`, `LogosAPIProvider`) without directory scanning. The daemon also saves a client token via `TokenManager::instance().saveToken("cli_client", token)` so CLI clients can authenticate.

### Build integration

The `core_service_dispatch.cpp` file provides a manual `callMethod()` dispatch table and `getMethods()` metadata for `CoreServiceImpl`. Unlike dynamically loaded modules that use `logos-cpp-generator`, core_service uses a hand-written dispatch because it is statically linked into the daemon binary.

```cpp
// core_service_dispatch.cpp — maps method names to CoreServiceImpl methods
QVariant CoreServiceImpl::callMethod(const QString& method, const QVariantList& args) {
    if (method == "loadModule") return loadModule(args.value(0).toString());
    if (method == "shutdown") return QVariant::fromValue(shutdown());
    // ... etc
}
```

---

## Components

### main.cpp (entry point)

**Files:** `src/main.cpp`

**Purpose:** Detect execution mode and dispatch to the appropriate path.

**API:**

| Function | Description |
|----------|-------------|
| `detectMode(argc, argv) -> Mode` | Returns `Daemon`, `Client`, or `Inline` |
| `main(argc, argv) -> int` | Dispatch to Daemon::start, Client command, or inline flow |

### Daemon

**Files:** `src/daemon/daemon.cpp/h`

**Purpose:** Manage the daemon lifecycle: start liblogos, register core_service, write connection file, handle signals for clean shutdown.

**API:**

| Method | Description |
|--------|-------------|
| `Daemon::start(modulesDirs) -> int` | Init liblogos, register core_service, write connection file, run event loop |
| `Daemon::setupSignalHandlers()` | Handle SIGINT/SIGTERM for clean shutdown |

### ConnectionFile

**Files:** `src/daemon/connection_file.cpp/h`

**Purpose:** Write, read, and clean up the daemon connection file (`<configDir>/daemon.json`). Pure parse — no liveness check.

**API:**

| Method | Description |
|--------|-------------|
| `ConnectionFile::write(instanceId, token, pid, modulesDirs, transports)` | Write connection file. `transports` is a vector of `TransportInfo` describing every opened listener. |
| `ConnectionFile::read() -> ConnectionInfo` | Parse the file. `ConnectionInfo::fileOk` is true iff the file exists and holds a non-empty `instance_id` — says nothing about liveness. |
| `ConnectionFile::remove()` | Remove connection file. |

**Connection file format** (`<configDir>/daemon.json`):

```json
{
  "instance_id": "a3f1c8d20b4e",
  "token": "550e8400-e29b-41d4-a716-446655440000",
  "pid": 12345,
  "started_at": "2026-03-23T14:00:00Z",
  "modules_dirs": ["/path/to/modules"],
  "transports": [
    { "protocol": "local" },
    { "protocol": "tcp",     "host": "0.0.0.0", "port": 6000, "codec": "json" },
    { "protocol": "tcp_ssl", "host": "0.0.0.0", "port": 6443,
      "codec": "cbor", "ca_file": "/etc/logoscore/ca.pem",
      "verify_peer": true }
  ]
}
```

`transports` is optional. If absent (old daemons), the client assumes a single `local` listener.

### ClientConnection

**Files:** `src/client/client_connection.cpp/h`

**Purpose:** Single place that resolves "what endpoint should the client dial" from the daemon-advertised `TransportInfo` plus client-side env overrides. Used by both `Client::connect()` (for normal RPC) and internally anywhere else the client needs to reach the same daemon.

**API:**

| Function | Description |
|----------|-------------|
| `ClientConnection::effectiveTransport(advertised)` | For `tcp` / `tcp_ssl` entries: apply `LOGOSCORE_CLIENT_TCP_HOST`, `LOGOSCORE_CLIENT_TCP_PORT`, `LOGOSCORE_CLIENT_NO_VERIFY_PEER` overrides. For `local`: return unchanged. Out-of-range port env vars are silently dropped so the subsequent connect reports the clearer error. |

The overrides exist so the daemon can publish a "bind address" like
`0.0.0.0:6000` while the client on the other side of a NAT / docker port map
/ SSH tunnel dials something different — without either side having to write
that divergence into `daemon.json`.

### TokenStore

**Files:** `src/daemon/token_store.cpp/h`

**Purpose:** Persistent store for named client tokens. Plaintext tokens are never stored — only hashes in `tokens.db` for lookup.

**API:**

| Method | Description |
|--------|-------------|
| `TokenStore(configDir)` | Opens `<configDir>/tokens.db`. |
| `issueToken(name, replace) -> optional<string>` | Mint a new token, record its digest under `name`, write `tokens/<name>.json` with the raw token + endpoint info, return the raw token. Fails (returns nullopt) if `name` already exists and `replace` is false. |
| `revokeToken(name) -> bool` | Remove the `name` entry from `tokens.db` and delete `tokens/<name>.json`. |
| `listTokens() -> vector<IssuedToken>` | Enumerate `{name, issued_at}` — never plaintext, never the digest. |
| `lookupByToken(token) -> optional<Entry>` | Daemon-side: validate an incoming token against the stored digests. |

The on-disk digest is a SHA-256 hex string — collision-resistant by design so
two distinct tokens can never validate to the same name. It's used internally
for lookup/validation only; it's not exposed via `IssuedToken`. The only place
the raw token ever lives is in `tokens/<name>.json` at the moment of issuance;
treat that file like a private key. tokens.db and the per-client files are
written with mode 0600.

**How the client finds the daemon:**

The logos-cpp-sdk uses `LogosInstance::id(moduleName)` to build registry URLs in the format `local:logos_{moduleName}_{instanceId}`. The instance ID is a 12-char UUID prefix shared by all processes in the same daemon tree (via the `LOGOS_INSTANCE_ID` env var). Child processes (like `logos_host`) inherit it automatically.

The CLI client is **not** a child process of the daemon — it's a separate invocation. So it cannot inherit the env var. Instead:

1. Daemon starts → `LogosInstance::id()` generates `a3f1c8d20b4e` → sets `LOGOS_INSTANCE_ID`
2. core_service registers at `local:logos_core_service_a3f1c8d20b4e`
3. Daemon writes `instance_id` to `~/.logoscore/daemon.json`
4. Client reads `instance_id` from the file → sets `LOGOS_INSTANCE_ID=a3f1c8d20b4e` in its own process → now `LogosInstance::id("core_service")` returns the matching URL
5. Client connects via `LogosAPIClient` → reaches the correct daemon

This is why the connection file stores `instance_id` rather than a hardcoded registry URL — the client uses it to reconstruct URLs using the same `LogosInstance::id()` function the SDK uses internally.

The token is a UUID generated on daemon startup and stored in the TokenManager. Client commands read it automatically for local auth. For remote/CI usage, the token can be passed via `LOGOSCORE_TOKEN` env var or `~/.logoscore/config.json`.

### Client

**Files:** `src/client/client.cpp/h`

**Purpose:** Connect to the daemon's `core_service` module via `LogosAPIClient` and invoke its LOGOS_METHODs.

The client is a thin wrapper around `LogosAPIClient`. Each method maps 1:1 to a LOGOS_METHOD on `core_service`:

**API:**

| Method | core_service method called |
|--------|---------------------------|
| `Client::connect() -> bool` | Read connection file, set `LOGOS_INSTANCE_ID` from `instance_id`, create `LogosAPIClient` targeting `"core_service"`, authenticate |
| `Client::isConnected() -> bool` | — |
| `Client::loadModule(name) -> QVariant` | `core_service.loadModule(name)` |
| `Client::unloadModule(name) -> QVariant` | `core_service.unloadModule(name)` |
| `Client::reloadModule(name) -> QVariant` | `core_service.reloadModule(name)` |
| `Client::listModules(filter) -> QJsonArray` | `core_service.listModules(filter)` |
| `Client::getStatus() -> QJsonObject` | `core_service.getStatus()` |
| `Client::getModuleInfo(name) -> QJsonObject` | `core_service.getModuleInfo(name)` |
| `Client::getModuleStats() -> QJsonArray` | `core_service.getModuleStats()` |
| `Client::callModuleMethod(module, method, args) -> QVariant` | `core_service.callModuleMethod(module, method, args)` |
| `Client::shutdown() -> QJsonObject` | `core_service.shutdown()` |
| `Client::watchModuleEvents(module, event, callback)` | `core_service.watchModuleEvents(module, event)` + event subscription |

**Implementation pattern:**

```cpp
QVariant Client::loadModule(const QString& name) {
    return m_apiClient->invokeRemoteMethod("core_service", "loadModule", name);
}
```

### Output

**Files:** `src/client/output.cpp/h`

**Purpose:** Format output for human or JSON consumption. Detects TTY status for automatic mode selection.

**API:**

| Method | Description |
|--------|-------------|
| `Output::isTTY() -> bool` | Check if stdout is a terminal |
| `Output::isJsonMode() -> bool` | Check if JSON output is active (flag or non-TTY) |
| `Output::printSuccess(data)` | Print success result (human table or JSON) |
| `Output::printError(code, message)` | Print error to stderr (human) or JSON to stdout |
| `Output::printList(items)` | Print a list (table or JSON array) |
| `Output::printEvent(event)` | Print a single event (formatted line or NDJSON) |

### Config

**Files:** `src/config.cpp/h`

**Purpose:** Read authentication credentials from environment variables and config files.

**API:**

| Method | Description |
|--------|-------------|
| `Config::getToken() -> QString` | Resolve token: env var → config file → connection file |
| `Config::load() -> Config` | Load `<configDir>/config.json` |
| `Config::configDir() -> QString` | Resolve config dir: explicit setter (`--config-dir`) → `LOGOSCORE_CONFIG_DIR` env → `~/.logoscore` |
| `Config::setConfigDir(QString)` | Process-wide override set from `main` when `--config-dir` is passed |

**Token resolution order:**
1. `LOGOSCORE_TOKEN` environment variable
2. `<configDir>/config.json` `{"token": "..."}`
3. `<configDir>/daemon.json` `{"token": "..."}`

**Config dir resolution order:**
1. `--config-dir <path>` CLI flag (sets process-wide override, mirrors into `LOGOSCORE_CONFIG_DIR`)
2. `LOGOSCORE_CONFIG_DIR` environment variable
3. `~/.logoscore` (default)

Parallel daemons run side-by-side when invoked with distinct `--config-dir` values; client commands must target the daemon by passing the same `--config-dir`.

### Command Base Class

**Files:** `src/client/commands/command.cpp/h`

**Purpose:** Base class for all client subcommand implementations.

**API:**

| Method | Description |
|--------|-------------|
| `Command::execute(args) -> int` | Run the command, return exit code |
| `Command::client() -> Client&` | Access the core_service client |
| `Command::output() -> Output&` | Access the output formatter |

### Inline Components

**CommandLineParser** (`src/inline/command_line_parser.cpp/h`):
Parses `-m`, `-l`, `-c`, `--quit-on-finish` flags. Returns `CoreArgs` struct.

**CallExecutor** (`src/inline/call_executor.cpp/h`):
Executes method calls via `logos_core_call_plugin_method_async()` with 30s timeout. Supports `@file` parameter resolution.

---

## CLI Commands

All client-path commands connect to the daemon's `core_service` module via `LogosAPIClient` and call its LOGOS_METHODs. They never call liblogos C API functions directly.

### logoscore daemon

Start the daemon process. **This is the only command that runs the daemon path.**

```
logoscore -D [--modules-dir <path>]...
logoscore daemon [--modules-dir <path>]...
```

**Behavior:**
1. `logos_core_init(argc, argv)`, add plugin directories, `logos_core_start()`
2. Register `core_service` in-process via `logos_core_register_module()`
3. Write `~/.logoscore/daemon.json` (with token)
4. `logos_core_exec()` (Qt event loop — blocks)
5. On SIGINT/SIGTERM: `logos_core_cleanup()`, remove connection file, exit

**Exit codes:** 0 on clean shutdown, 1 on error.

### logoscore load-module

Load a module into the running daemon.

```
logoscore load-module <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.loadModule(name)`
3. Prints result and exits

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found or load failed.

### logoscore unload-module

Unload a module from the running daemon.

```
logoscore unload-module <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.unloadModule(name)`

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found or unload failed.

### logoscore list-modules

List available or loaded modules.

```
logoscore list-modules [--loaded]
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.listModules(filter)` — filter is `"loaded"` or `"all"`
3. Returns all modules with status enum (`loaded | not_loaded | crashed | loading`)
4. Formats and prints result with NAME, VERSION, STATUS, UPTIME columns
5. Crash metadata (`exit_code`, `crashed_at`, `crash_reason`) is included in JSON for crashed modules

**Exit codes:** 0 on success, 2 if no daemon.

### logoscore status

Show overall daemon and module health.

```
logoscore status
```

**Behavior:**
1. Reads `<configDir>/daemon.json`. If missing or unparseable, prints "not running" and exits with code 1 (no point trying to connect).
2. Otherwise tries to connect and call `core_service.getStatus()`. The RPC call IS the liveness check — there's no separate cheap probe, because no cheap probe is correct across every transport (local Unix socket vs remote TCP across NAT is a meaningless question for PID-based liveness).
3. On RPC timeout / connect refused: reports "not running" with the error reason, exits with code 1.
4. On success: displays daemon info (PID, uptime, version, instance ID) and all module statuses with summary counts.

**Exit codes:** 0 on success, 1 if daemon not running (uses 1 not 2 because the status command itself succeeded — it's reporting the state, not failing to connect).

### logoscore reload-module

Unload and re-load a module.

```
logoscore reload-module <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.reloadModule(name)` — core_service handles the unload/load logic internally, including fallback to plain load if module isn't currently loaded
3. Returns result with `previous_status` field

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found or reload failed.

### logoscore module-info

Show detailed information about a specific module.

```
logoscore module-info <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.getModuleInfo(name)`
3. For loaded modules: displays name, version, status, PID, uptime, dependencies, available methods
4. For crashed modules: displays name, version, status, exit code, crash signal, crashed_at, restart count, last log line, PID before crash
5. For not-loaded modules: displays name, version, status, dependencies

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found.

### logoscore call

Call a method on a loaded module.

```
logoscore call <module> <method> [args...]
```

Alternative syntax:

```
logoscore module <name> method <method> [args...]
```

**Behavior:**
1. Connects to daemon via `Client`
2. Resolves `@file` arguments to file contents
3. Type-coerces arguments: numeric strings → int/double, `"true"`/`"false"` → bool, rest → string
4. Calls `core_service.callModuleMethod(module, method, args)` — core_service proxies the call to the target module via `LogosAPIClient`
5. In human mode: prints scalar results as plain values, structured results as indented JSON, null produces no output. In JSON mode: prints the full result envelope.

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not loaded, 4 if method not found or call failed.

### logoscore watch

Watch events from a loaded module.

```
logoscore watch <module> [--event <name>]
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.watchModuleEvents(module, event)` — core_service registers an event listener on the target module and forwards events through its own event system
3. Client subscribes to core_service events via `LogosAPIClient::onEvent()`
4. On each event: prints formatted line (human) or NDJSON line (JSON mode)
5. Runs until SIGINT/SIGTERM

**Exit codes:** 0 on clean shutdown, 2 if no daemon, 3 if module not loaded.

### logoscore stats

Show resource usage for loaded modules.

```
logoscore stats
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.getModuleStats()`
3. Formats as table (human) or JSON array

**Exit codes:** 0 on success, 2 if no daemon.

### logoscore stop

Stop the running daemon via RPC.

```
logoscore stop
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.shutdown()`
3. core_service schedules `QCoreApplication::quit()` after 200ms delay
4. If the RPC response arrives: prints success and exits
5. If the daemon exits before the response (RPC_FAILED): treats it as success — the daemon is already gone

**Exit codes:** 0 on success (including when daemon exits before response), 2 if no daemon.

### logoscore info

Alias for `module-info`. Delegates to `module-info` command.

```
logoscore info <module>
```

**Behavior:** Same as `module-info <module>` — see above.

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found.

---

## Call Chain: CLI → core_service → liblogos

Client commands never call liblogos functions directly. The full call chain is:

```
CLI client                    core_service (daemon-side)              liblogos C API
─────────                     ─────────────────────────               ──────────────
logoscore load-module waku
  → Client::loadModule("waku")
    → LogosAPIClient::invokeRemoteMethod(
        "core_service", "loadModule", "waku")
      ───── IPC (Qt Remote Objects) ─────→
                                          CoreServiceImpl::loadModule("waku")
                                            → logos_core_load_plugin_with_dependencies("waku")
                                            → build result JSON
      ←──── IPC (return value) ──────────
    → Output::printSuccess(result)
    → exit(0)
```

### Daemon path — liblogos usage

Only the daemon path calls liblogos C API functions directly:

| Daemon operation | liblogos functions |
|---|---|
| Start core | `logos_core_init`, `logos_core_add_plugins_dir`, `logos_core_start` |
| Register core_service | `LogosAPI`, `LogosAPIProvider::registerObject` (C++ SDK) |
| Run event loop | `logos_core_exec` |
| Shutdown | `logos_core_cleanup` |

### Client path — core_service method mapping

Client commands call core_service LOGOS_METHODs, which delegate to liblogos internally:

| CLI command | core_service method | liblogos function called internally |
|---|---|---|
| `load-module` | `loadModule(name)` | `logos_core_load_plugin_with_dependencies` |
| `unload-module` | `unloadModule(name)` | `logos_core_unload_plugin` |
| `reload-module` | `reloadModule(name)` | `logos_core_unload_plugin` + `logos_core_load_plugin_with_dependencies` |
| `list-modules` | `listModules(filter)` | `logos_core_get_known_plugins`, `logos_core_get_loaded_plugins` |
| `status` | `getStatus()` | reads daemon state + `listModules` |
| `module-info` | `getModuleInfo(name)` | plugin metadata + methods introspection |
| `call` | `callModuleMethod(module, method, args)` | `LogosAPIClient::invokeRemoteMethod` (proxied to target module) |
| `watch` | `watchModuleEvents(module, event)` | `LogosAPIClient::onEvent` (forwarded) |
| `stats` | `getModuleStats()` | `logos_core_get_module_stats` |
| `stop` | `shutdown()` | `QTimer::singleShot(200, ..., &QCoreApplication::quit)` |
| `info` | alias for `module-info` | — |

### Inline path — direct liblogos

Inline mode (`-m -l -c`) calls liblogos functions directly in the same process. No daemon, no core_service.

---

## Build

### Nix

```bash
nix build

# The logoscore binary is at:
./result/bin/logoscore

# Run daemon
./result/bin/logoscore -D -m /path/to/modules
```

---

## Examples

### Basic Usage

```bash
# Start the daemon with module directories
logoscore -D -m ./modules &

# Check daemon health
logoscore status

# Load modules
logoscore load-module waku
logoscore load-module chat

# List loaded modules (with status and uptime)
logoscore list-modules --loaded

# Get detailed module info
logoscore module-info chat

# Call a method
logoscore call chat send_message "hello world"

# Reload a crashed module
logoscore reload-module chat

# Watch events
logoscore watch chat --event chat-message

# Get stats
logoscore stats

# Stop daemon
logoscore stop
```

### Agent / Script Usage

```bash
# Start daemon
logoscore -D -m ./modules &
sleep 2

# Preflight: verify daemon is running
logoscore status --json | jq -e '.daemon.status == "running"' > /dev/null

# Check what's available and their state
logoscore list-modules --json
# [
#   {"name":"waku","version":"0.1.0","status":"not_loaded"},
#   {"name":"chat","version":"0.2.0","status":"not_loaded"}
# ]

# Load modules (JSON output for parsing)
logoscore load-module waku --json
# {"status":"ok","module":"waku","version":"0.1.0","dependencies_loaded":["store"]}

logoscore load-module chat --json
# {"status":"ok","module":"chat","version":"0.2.0","dependencies_loaded":[]}

# Discover methods before calling
logoscore module-info chat --json | jq '.methods[].name'
# "send_message"
# "get_history"
# "get_status"

# Call method and parse result
RESULT=$(logoscore call chat send_message "hello" --json)
echo "$RESULT" | jq -r '.result'

# Handle crashed modules
MODULE_STATUS=$(logoscore status --json | jq -r '.modules[] | select(.name=="chat") | .status')
if [ "$MODULE_STATUS" = "crashed" ]; then
  logoscore module-info chat --json | jq '{exit_code, crash_signal, restart_count}'
  logoscore reload-module chat --json
fi

# Stream events to log file
logoscore watch chat --event chat-message --json >> events.log &
WATCH_PID=$!

# Check overall health before cleanup
logoscore status --json | jq '.modules_summary'
# {"loaded": 3, "crashed": 0, "not_loaded": 0}

# Cleanup
kill $WATCH_PID
logoscore stop
```

### Using Environment Variables for Auth

```bash
# Set token via environment
export LOGOSCORE_TOKEN=xyz123

# Or inline per-command
LOGOSCORE_TOKEN=xyz123 logoscore load-module waku

# Or via config file
mkdir -p ~/.logoscore
echo '{"token": "xyz123"}' > ~/.logoscore/config.json
logoscore load-module waku
```

### Inline Mode

```bash
# Single-process mode — no daemon needed
logoscore -m ./modules -l waku,chat -c "chat.send_message(hello)" --quit-on-finish
```

### Piping and Composition

```bash
# Filter loaded modules
logoscore list-modules --json | jq '[.[] | select(.status == "loaded")]'

# Find crashed modules
logoscore list-modules --json | jq '[.[] | select(.status == "crashed")]'

# Watch events and filter
logoscore watch chat --event chat-message --json | jq 'select(.data.from == "alice")'

# Monitor module health with status dashboard
watch -n 5 'logoscore status --json | jq "{daemon: .daemon.status, modules: .modules_summary}"'

# Monitor resource usage
watch -n 5 'logoscore stats --json | jq ".[] | {name, cpu_percent, memory_mb}"'

# Auto-reload crashed modules
logoscore list-modules --json | jq -r '.[] | select(.status == "crashed") | .name' | while read mod; do
  logoscore reload-module "$mod" --json
done
```

---

## Tests

| Test File | Coverage |
|-----------|----------|
| `test_commands.cpp` | All subcommand implementations via mock client: load/unload/reload module, list-modules, status, module-info, call, stats, watch, stop. Tests both success and error paths, JSON and human output modes. |
| `test_mode_detection.cpp` | Mode detection (daemon/client/inline/help/version), known subcommands list, argument parsing. |
| `test_output.cpp` | Output formatter (human/JSON), TTY detection, printSuccess/printError/printRaw. |
| `test_connection_file.cpp` | Round-trip the on-disk file — instance_id, token, pid, modulesDirs, and per-protocol `transports` entries (local/tcp/tcp_ssl, codec defaulting, omitted-transports back-compat). `fileOk` is independent of the pid (it's a parse check, not liveness). |
| `test_client_connection.cpp` | `ClientConnection::effectiveTransport`: local passthrough, tcp host/port overrides, tcp_ssl `--no-verify-peer`, out-of-range port dropped silently. |
| `test_token_store.cpp` | Token issuance, duplicate-name rejection (unless `--replace`), revocation, list, persistence round-trip. Confirms `tokens.db` stores hashes only; plaintext lives in the client file. |
| `test_config.cpp` | Token resolution order (env var → config file → connection file). |
| `test_cli.cpp` | End-to-end CLI tests: help, version, no-args, client commands without daemon, inline mode with --verbose. |

---

## Known Issues

1. **Event forwarding** — The `watch` command requires `core_service` to forward events from target modules to CLI clients. The approach is: `core_service.watchModuleEvents()` registers a listener on the target module via `LogosAPIClient::onEvent()`, then re-emits received events via `LogosProviderBase::emitEvent()`. The CLI client subscribes to `core_service` events. This creates a relay chain (target module → core_service → CLI client) which adds latency. An alternative would be having the CLI client connect directly to the target module, but that bypasses the core_service gateway pattern.

2. **Stale connection file** — If the daemon crashes without removing `<configDir>/daemon.json`, the file stays on disk. Clients no longer pre-probe PID liveness (that only works for local daemons); instead the first RPC fails with a connect error and the `status` command turns that into a "not running" report. The only cost of a stale file is that the first attempt after a crash wastes one RPC timeout; in practice that's fine.

3. **Crash tracking** — The daemon needs to track module crash metadata (exit code, signal, timestamp, restart count, last log line) so that `listModules` and `getModuleInfo` on core_service can report it. This may require extending liblogos to expose crash info, or core_service could track it independently by monitoring `QProcess` signals.

4. **callModuleMethod proxy** — When `core_service` proxies calls to target modules via `LogosAPIClient`, it needs the target module's auth token. The daemon's TokenManager has all tokens, but core_service must obtain them. This may require core_service to have a privileged token or to be pre-authorized for all modules.

## Future Improvements

1. **Tab completion** — Shell completion scripts for bash/zsh/fish.
2. **TUI mode** — Interactive terminal UI with autocomplete (like Obsidian CLI).
3. **Batch mode** — Execute multiple commands from a file (`logoscore batch commands.txt`).
4. **`module-logs` command** — Stream or tail module process logs (`logoscore module-logs chat --tail 50`). Referenced by error messages but not yet specified.
5. **Extract core_service** — If core_service grows, it could be extracted into a standalone plugin loaded from disk rather than statically linked. The LOGOS_PROVIDER API makes this trivial.
6. **Capability-scoped tokens** — Today all tokens are admin-equivalent. Named tokens (`issue-token --name …`) create separate identities but each one is still fully authorised against the daemon. A scope/capability system would let e.g. a read-only token call `list-modules` / `status` but reject `load-module` / `stop`.
7. **Client-cert TLS** — The `tcp_ssl` transport today authenticates the daemon to the client (server cert); mutual TLS + client-cert auth would be a natural extension once we have scoped tokens, and subsumes the token-file distribution problem for many deployments.

