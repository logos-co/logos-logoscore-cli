# Logosctl CLI — Project Description

## Overview

The `logosctl` CLI is a standalone application that provides the command-line interface for the Logos Core runtime. It depends on **liblogos**, a C library that provides the core runtime (plugin discovery, loading, dependency resolution, event loop). The CLI is responsible for:

- Running as a daemon that hosts the liblogos runtime
- Providing client commands that talk to the daemon via RPC

This project will live in its own repository. liblogos is consumed as an external C library dependency.

## Project Structure

```
logos-logoscore-cli/
├── src/                              # All CLI source code
│   ├── main.cpp                      # logosctl entry point — detects mode, dispatches
│   ├── main_legacy.cpp               # logoscore entry point — same daemon/client/core_service
│   │                                 # code below, frozen surface (see the README)
│   ├── config.cpp/h                  # Token + config file resolution
│   ├── paths.cpp/h                   # Executable/bundle-relative path resolution (no Qt)
│   │
│   ├── daemon/                       # Daemon path (logosctl daemon start)
│   │   ├── daemon.cpp/h              # Start core, register core_service through the
│   │   │                             # plain C ABI, wait for shutdown
│   │   ├── daemon_state.cpp/h        # DaemonConfig (config.json) + DaemonRuntimeState
│   │   │                             # (state.json) — operator preferences (writes only
│   │   │                             # on --persist-config) + live runtime state.
│   │   ├── access_policy_arg.cpp/h   # Resolve --access-policy into the JSON document
│   │   │                             # handed to logos_core_set_access_policy()
│   │   ├── log_sink.cpp/h            # Pipe-based capture of daemon + module-host
│   │   │                             # stdout/stderr into a rotating log file
│   │   ├── port_allocator.cpp/h      # Legacy network-config validation support
│   │   └── token_store.cpp/h         # Named-token table — TokensFile owns daemon/tokens.json
│   │                                 # (hashed entries) + raw daemon/tokens/<name>.json
│   │
│   ├── client/                       # Client path (all subcommands)
│   │   ├── client.cpp/h              # Client interface + RpcClient — connect to the
│   │   │                             # daemon's core_service via PlainRpcClient
│   │   ├── client_state.cpp/h        # Read/write <configDir>/client/config.json (dial spec)
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
│   │       ├── package_command.cpp/h        # install / remove / update (plan + apply)
│   │       ├── catalog_command.cpp/h        # Browse + download from the online catalog
│   │       ├── config_command.cpp/h         # Inspect/edit the config tree
│   │       ├── issue_token_command.cpp/h    # Mints named tokens (daemon/tokens/<name>.json)
│   │       ├── revoke_token_command.cpp/h   # Revokes by name
│   │       └── list_tokens_command.cpp/h    # Lists issued tokens (name + metadata, no plaintext)
│   │
│   └── core_service/                 # Built-in module — CLI ↔ daemon RPC gateway
│       ├── core_service_impl.h       # Plain C++ service implementation
│       ├── core_service_impl.cpp     # Method implementations (delegates to liblogos C API)
│       ├── package_ops.cpp/h         # Daemon-side plan/apply for package operations
│       ├── metadata.json             # Plugin metadata
│       └── core_service_dispatch.cpp # Hand-written callMethodStd/getMethodsStd dispatch
│                                     # (no core_service_loader.h: the daemon registers the
│                                     # object in-process, it is never discovered as a plugin)
│
├── tests/
│   ├── test_commands.cpp             # Subcommands against a mock Client
│   ├── test_mode_detection.cpp       # Mode detection, subcommand dispatch
│   ├── test_output.cpp               # Output formatter tests
│   ├── test_daemon_state.cpp         # daemon/state.json + tokens round-trip
│   ├── test_token_store.cpp          # Token issue / revoke / list / persistence
│   ├── test_config.cpp               # Token + config-dir resolution
│   ├── test_paths.cpp                # Executable/bundle path resolution
│   ├── test_port_allocator.cpp       # Ephemeral-port allocation
│   ├── test_access_policy_arg.cpp    # --access-policy argument resolution
│   ├── test_log_sink.cpp             # Log capture + rotation
│   ├── test_cli.cpp                  # End-to-end logosctl CLI
│   ├── test_integration.cpp          # logosctl against a live daemon
│   ├── test_cli_logoscore.cpp        # End-to-end logoscore CLI (frozen duplicate)
│   └── test_integration_logoscore.cpp # logoscore against a live daemon (frozen duplicate)
│
├── docs/
│   ├── index.md                      # Doc index
│   ├── spec.md                       # CLI specification (user-facing behavior)
│   ├── project.md                    # This file (implementation details)
│   ├── logoscore.md                  # logoscore user guide
│   └── logosctl.md                   # logosctl user guide
│
├── doctests/                         # Executable documentation specs
├── CMakeLists.txt                    # Build configuration
├── flake.nix                         # Nix flake
└── nix/                              # Nix build modules
```

## Dependencies

| Dependency | Type | Purpose |
|---|---|---|
| **liblogos** | C library (external) | Core runtime: module discovery, loading, dependency resolution and process stats |
| **logos-cpp-sdk headers** | C++ headers (external) | Qt-free JSON/result value aliases used by the command surface |
| **logos-protocol-plain** | Shared C++ library (external) | Qt-free `lp_*` client/provider ABI and the `qt_remote_plain` transport |
| **logos_host_qt** | Child executable supplied by liblogos | Compatibility loader and runtime for current Qt plugins; Qt stays in this separate process |
| **CMake 3.14+** | Build system | — |
| **Google Test** | Test framework | — |
| **Nix** | Package manager | Reproducible builds |

**No code generator is in this build.** `core_service` used to be listed here
as depending on `logos-cpp-generator` to emit a `LOGOS_METHOD` dispatch table;
that marker macro and that generator mode are gone from this repo's path —
`core_service_dispatch.cpp` is hand-written (see *Build integration* below).

### liblogos C API surface used

The CLI uses these functions from liblogos (declared in `logos_core.h`):

| Function | Used by |
|---|---|
| `logos_core_init(argc, argv)` | Daemon |
| `logos_core_add_modules_dir(path)` | Daemon |
| `logos_core_start()` | Daemon |
| `logos_core_cleanup()` | Daemon |
| `logos_core_load_module(name, LOGOS_LOAD_REQUIRED_AND_OPTIONAL)` | Daemon, core_service |
| `logos_core_unload_module(name, false)` | core_service |
| `logos_core_get_known_modules()` | core_service |
| `logos_core_get_loaded_modules()` | core_service |
| `logos_core_get_modules_info()` | core_service |
| `logos_core_get_module_stats()` | core_service |

---

## CLI Execution Paths

The `logosctl` binary detects its mode from the first argument and dispatches to one of two paths:

```
logosctl daemon start / daemon         →  Daemon path    (long-running, hosts modules)
logosctl <subcommand>        →  Client path    (short-lived, talks to daemon)
```

### Detection logic (main.cpp)

Before mode detection, `main()` scans argv for `-v`/`--verbose`. Daemon and
module-host logging is handled by `LogSink` and spdlog; the CLI installs no Qt
message handler.

```
if argv contains "-D" or "daemon"      → daemon path
else if argv[1] is a known subcommand  → client path
else if argv contains -m/-p (no -D)    → error (inline mode removed)
else                                   → print help
```

### Daemon Path (`logosctl daemon start`)

```
main.cpp
  → Daemon::start(...)
    1. Claim the config directory and set LOGOS_INSTANCE_ID
    2. Initialize liblogos, add module directories, and discover modules
    3. Create CoreServiceImpl and publish it with lp_provider over
       qt_remote_plain
    4. Adopt liblogos' capability credential, install the persistent-token
       validator, and save the boot token in the provider
    5. Write daemon/state.json plus the local client config and token
    6. Wait on a condition variable until SIGINT, SIGTERM, or shutdown()
    7. Destroy the provider, clean up liblogos, and remove state.json
```

The daemon uses liblogos only for module discovery and lifecycle. Current Qt
plugins still run in separate `logos_host_qt` child processes. The daemon,
core service, and client use the shared `logos_protocol_plain` runtime and do
not load Qt.

The Qt-free C ABI exposes local `qt_remote_plain` plus `tcp` and `tcp_ssl`
providers and clients. The daemon's resolved transport set supplies listener
addresses; the client config supplies dial addresses and TLS verification.

Named tokens are persisted as SHA-256 digests by `TokenStore`. The provider's
token-validator callback consults that store on demand, which lets tokens
issued after startup authenticate without copying Qt token-manager state.

### Client Path (`logosctl <subcommand>`)

```
main.cpp
  → RpcClient::connect()
    1. Read client/config.json and the referenced token file
    2. Require the local transport and set LOGOS_INSTANCE_ID
    3. Save the credential through lp_token_save()
    4. Create an lp_client for core_service over qt_remote_plain
  → Command::execute(args)
    1. Invoke a core_service method with JSON arguments
    2. Format the JSON result and exit
```

Client commands do not call liblogos. They use the same Qt Remote Objects wire
format as current modules through the plain C ABI. The endpoint preflight
detects a definitely absent local socket before the first RPC, while ambiguous
conditions still proceed to the normal protocol timeout.

### Inline Path (removed)

The legacy inline path (`logosctl -m -l -c "module.method(args)" --quit-on-finish`)
started the core in the same short-lived process, loaded modules, executed the
`-c` calls directly via the C API, and exited. It has been removed — use a
daemon (`-D`) plus `load-module` / `call` client subcommands instead. The
daemon starts clean; `-m`/`--persistence-path` configure daemon startup only
(the `-l/--load-modules` autoload flag was also removed).

---

## CoreService Module

`core_service` is an ordinary C++ object owned by the daemon. It is not a Qt
object or a plugin. An `lp_provider` callback passes each incoming JSON request
to `CoreServiceImpl::callMethodStd()`; the metadata callback returns
`getMethodsStd()`.

```cpp
class CoreServiceImpl {
public:
    StdLogosResult loadModule(const std::string& name);
    StdLogosResult callModuleMethod(const std::string& module,
                                    const std::string& method,
                                    const LogosList& args);
    bool watchModuleEvents(const std::string& module,
                           const std::string& eventName);
    LogosMap shutdown();

    nlohmann::json callMethodStd(const std::string& method,
                                 const nlohmann::json& args);
    nlohmann::json getMethodsStd();

private:
    logosctl::PlainRpcContext m_rpc{"core_service"};
};
```

Lifecycle and query methods call the liblogos C API. Calls to loaded modules use
a cached `PlainRpcClient` targeting that module over `qt_remote_plain`.
Event watches use `lp_subscribe`; an empty event name subscribes to every
event and the core service forwards each one as `module_event`.

The hand-written dispatcher converts argument errors to a structured
`INVALID_ARGS` response. Shutdown schedules `Daemon::requestShutdownAfter()`
so the provider can serialize the reply before the daemon tears it down.

Daemon-side registration is entirely C ABI based:

```cpp
lp_provider* provider = lp_provider_create("core_service",
                                           kPlainLocalTransport);
lp_provider_set_token_validator(provider, validateToken, &tokenStore);
lp_provider_register(provider, dispatchCall, methodMetadata,
                     emitEvent, coreService);
lp_provider_save_token(provider, "cli_client", autoToken.c_str());
```

`src/core_service/metadata.json` remains a declarative identity document; the
daemon does not load it as a plugin.

## Components

### main.cpp (entry point)

**Files:** `src/main.cpp`

**Purpose:** Detect execution mode and dispatch to the appropriate path.

**API:**

| Function | Description |
|----------|-------------|
| `detectMode(argc, argv) -> Mode` | Returns `Daemon` or `Client` |
| `main(argc, argv) -> int` | Dispatch to Daemon::start or a Client command |

### Daemon

**Files:** `src/daemon/daemon.cpp/h`

**Purpose:** Manage the daemon lifecycle: start liblogos, register core_service, write the daemon state file, emit the local-default client config, handle signals for clean shutdown.

**API:**

| Method | Description |
|--------|-------------|
| `Daemon::start(modulesDirs) -> int` | Init liblogos, register core_service, write `daemon/state.json` + emit `client/config.json` and `client/auto.json`, wait for the shutdown signal |
| `Daemon::setupSignalHandlers()` | Handle SIGINT/SIGTERM for clean shutdown |

### DaemonConfigFile + DaemonRuntimeStateFile

**Files:** `src/daemon/daemon_state.cpp/h`

The daemon writes `daemon/state.json` after its provider is live and removes
it during clean shutdown. The state includes the instance id, process id,
module directories, and the local transport advertised for `core_service`
and `capability_module`. `daemon/config.json` retains operator intent only
when `--persist-config` is used.

The configuration schema accepts local and network transport records; the
Qt-free daemon passes them to the matching plain protocol providers.

### TokenStore

**Files:** `src/daemon/token_store.cpp/h`

`TokenStore` persists issued token names, SHA-256 digests, issue times,
expiry, and local-only policy. Raw named tokens are written separately with
mode 0600 for distribution. The provider validator calls
`lookupByToken()` for every credential that is not already in its in-memory
boot-token table, so issue and revoke operations take effect immediately.

### ClientStateFile

**Files:** `src/client/client_state.cpp/h`

The local client file identifies the daemon instance, names the token file, and
contains a `core_service` transport entry:

```json
{
  "version": 2,
  "token_file": "auto.json",
  "instance_id": "a3f1c8d20b4e",
  "daemon": {
    "core_service": { "transport": "local" },
    "capability_module": { "transport": "local" }
  }
}
```

The parser keeps the prior strict schema checks. `RpcClient::connect()`
currently requires both active entries to be `local`.

### Client

**Files:** `src/client/client.cpp/h`

`Client` is the mockable command interface. `RpcClient` implements it with
`PlainRpcClient`, which owns an `lp_client`, invokes JSON methods, and keeps
event subscription callbacks alive. It links `logos_protocol_plain`; the Qt
client classes are absent from its source and link closure.

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

**Purpose:** Read authentication credentials from environment variables and the client's dial-spec file.

**API:**

| Method | Description |
|--------|-------------|
| `Config::getToken() -> std::string` | Token resolution: only `LOGOSCTL_TOKEN` env var. Filesystem fallback (`client/<token_file>`) lives in `ClientStateFile::readTokenFile` since it requires parsing the client config. |
| `Config::configDir() -> std::string` | Resolve config dir: explicit setter (`--config-dir`) → `LOGOSCTL_CONFIG_DIR` env → `~/.logosctl` |
| `Config::setConfigDir(std::string)` | Process-wide override set from `main` when `--config-dir` is passed |
| `Config::daemonConfigPath() / daemonStatePath() / daemonTokensPath() / daemonTokensDir()` | Daemon-side path helpers under `<configDir>/daemon/` |
| `Config::clientConfigPath() / clientDir() / clientTokenPath(filename)` | Client-side path helpers under `<configDir>/client/`. `clientTokenPath` rejects any `filename` that isn't a plain name (contains `/`, `\`, or `..`) and resolves it to an in-`client/` sentinel, so an operator-influenced `token_file` value can't escape the dir to read an arbitrary file as a credential. |

The client dial spec lives in `client/config.json` and is loaded via `ClientStateFile::read()` (not `Config`). See the **ClientStateFile** section above for the schema and parsing contract.

**Token resolution order:**
1. `LOGOSCTL_TOKEN` environment variable
2. `<configDir>/client/<token_file>` (`token_file` defaults to `auto.json` when `client/config.json` doesn't override it)

**Config dir resolution order:**
1. `--config-dir <path>` CLI flag (sets process-wide override, mirrors into `LOGOSCTL_CONFIG_DIR`)
2. `LOGOSCTL_CONFIG_DIR` environment variable
3. `~/.logosctl` (default)

Parallel daemons run side-by-side when invoked with distinct `--config-dir` values; client commands must target the daemon by passing the same `--config-dir`. Two daemons may **not** share a config-dir: startup reads `daemon/state.json` and refuses (exit 1) if its recorded pid is still alive, since both would write the same `state.json` and either one's clean shutdown would unlink it out from under the other. A stale `state.json` left by a crashed daemon (pid no longer alive) is ignored and overwritten.

### Command Base Class

**Files:** `src/client/commands/command.cpp/h`

**Purpose:** Base class for all client subcommand implementations.

**API:**

| Method | Description |
|--------|-------------|
| `Command::execute(args) -> int` | Run the command, return exit code |
| `Command::client() -> Client&` | Access the core_service client |
| `Command::output() -> Output&` | Access the output formatter |
| `Command::ensureConnected() -> int` | Refuse a stale session, else connect. 0 on success; prints `NO_DAEMON` and returns 2 otherwise. The single door every RPC-opening command goes through |
| `detectStaleSession() -> optional<StaleSession>` | Free function. "This session's daemon is provably gone", from `daemon/state.json`'s pid and `instance_id`. `nullopt` for a live daemon, a foreign instance, or no state file — see [Client Path](#client-path-logosctl-subcommand) |

The companion check lives one layer down, in `RpcClient::connect()`:

| Function | Description |
|----------|-------------|
| `logosctl::localEndpointProvablyAbsent(module, instanceId, pathOut)` | `src/local_endpoint.h`, header-only. "A local dial cannot reach anyone": the socket file is missing, or it is there and refuses. Fails closed on everything else. Covers the cleanly-stopped session the pid guard cannot see |

---

## CLI Commands

All client-path commands connect to the daemon's `core_service` module through the plain protocol client and call its methods by name. They never call liblogos C API functions directly.

### logosctl daemon

Start the daemon process. **This is the only command that runs the daemon path.**

```
logosctl daemon start [--modules-dir <path>]...
logosctl daemon [--modules-dir <path>]...
```

**Behavior:**
1. Initialize liblogos, add module directories, and call `logos_core_start()`
2. Publish `core_service` with `lp_provider_register()`
3. Write `daemon/state.json` and the local client config/token
4. Wait for a signal or the `shutdown` RPC
5. Destroy the provider, clean up liblogos, remove `state.json`, and exit

**Exit codes:** 0 on clean shutdown, 1 on error.

### logosctl module load

Load a module into the running daemon.

```
logosctl module load <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.loadModule(name)`
3. Prints result and exits

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found or load failed.

### logosctl module unload

Unload a module from the running daemon.

```
logosctl module unload <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.unloadModule(name)`

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found or unload failed.

### logosctl module ls

List available or loaded modules.

```
logosctl module ls [--loaded]
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.listModules(filter)` — filter is `"loaded"` or `"all"`
3. Returns all modules with status enum (`loaded | not_loaded | crashed | loading`)
4. Formats and prints result with NAME, VERSION, STATUS, UPTIME columns
5. Crash metadata (`exit_code`, `crashed_at`, `crash_reason`) is included in JSON for crashed modules

An **unanswered** RPC is reported as `DAEMON_UNREACHABLE`, not as an empty
list. `Client::listModules` returns `optional<LogosList>` for exactly this
reason: it used to answer a failed call with `LogosList::array()`, so against
a daemon that was not running this printed `[]` and exited 0 — the one outcome
a script cannot argue with, since a healthy session with nothing loaded says
the same thing. `stats` had the identical bug and the identical fix.

**Exit codes:** 0 on success (including an empty list), 2 if no daemon or the
daemon did not answer.

### logosctl daemon status

Show overall daemon and module health.

```
logosctl daemon status
```

**Behavior:**
1. Reads `<configDir>/client/config.json` to learn how to dial. If missing or unparseable, prints "not running" and exits with code 1 (no point trying to connect).
2. Runs the same `detectStaleSession()` guard `ensureConnected()` does, one step earlier: a session whose own daemon's pid is gone reports `not_running` with the pid and the reason, exit 1. Earlier and separately because "no daemon" is an *answer* to `status`, not an error — the shared guard's `NO_DAEMON` / exit 2 would be the wrong shape.
3. Otherwise tries to connect and call `core_service.getStatus()`. The RPC call IS the liveness check — the endpoint preflight only reports absence when the local socket is definitely gone.
4. On RPC timeout / connect refused: reports "not running" with the error reason, exits with code 1.
5. On success: displays daemon info (PID, uptime, version, instance ID) and all module statuses with summary counts.

`status` connects directly rather than through `ensureConnected()`, because
that helper *prints* a `NO_DAEMON` error envelope on failure and this command's
answer to "no daemon" is a status report — going through it would put two JSON
documents on stdout for one command.

**Exit codes:** 0 on success, 1 if daemon not running (uses 1 not 2 because the status command itself succeeded — it's reporting the state, not failing to connect). A synthesized "not running" report — the one `RpcClient::getStatus` returns when the RPC produced no reply, marked with `rpc_error` — counts as not running and exits 1; it used to reach the success branch and exit 0, so the text said one thing and the exit code said another.

### logosctl module reload

Unload and re-load a module.

```
logosctl module reload <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.reloadModule(name)` — core_service handles the unload/load logic internally, including fallback to plain load if module isn't currently loaded
3. Returns result with `previous_status` field

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found or reload failed.

### logosctl module show

Show detailed information about a specific module.

```
logosctl module show <name>
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.getModuleInfo(name)`
3. For loaded modules: displays name, version, status, PID, uptime, dependencies, and available methods — each method shows its signature and, when documented, a `description` sourced from the method's header doc comment (carried in the module's `getPluginMethods` introspection)
4. For crashed modules: displays name, version, status, exit code, crash signal, crashed_at, restart count, last log line, PID before crash
5. For not-loaded modules: displays name, version, status, dependencies

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found.

### logosctl call

Call a method on a loaded module.

```
logosctl call <module> <method> [args...]
```

Alternative syntax:

```
logosctl module <name> method <method> [args...]
```

**Behavior:**
1. Connects to daemon via `Client`
2. Resolves `@file` arguments to file contents
3. Type-coerces arguments: numeric strings → int/double, `"true"`/`"false"` → bool, rest → string
4. Calls `core_service.callModuleMethod(module, method, args)` — core_service proxies the call to the target module through the plain protocol client
5. In human mode: prints scalar results as plain values, structured results as indented JSON, null produces no output. In JSON mode: prints the full result envelope.

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not loaded, 4 if method not found or call failed.

### logosctl watch

Watch events from a loaded module.

```
logosctl watch <module> [--event <name>]
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.watchModuleEvents(module, event)` — core_service registers an event listener on the target module and forwards events through its own event system
3. Client subscribes to core_service events via `lp_subscribe()`
4. On each event: prints formatted line (human) or NDJSON line (JSON mode)
5. Runs until SIGINT/SIGTERM

**Exit codes:** 0 on clean shutdown, 2 if no daemon, 3 if module not loaded.

### logosctl stats

Show resource usage for loaded modules.

```
logosctl stats
```

**Behavior:**
1. Connects to daemon via `Client`
2. Calls `core_service.getModuleStats()`
3. Formats as table (human) or JSON array

As with `module ls`, an unanswered RPC is `DAEMON_UNREACHABLE` rather than an
empty stats list.

**Exit codes:** 0 on success (including an empty list), 2 if no daemon or the
daemon did not answer.

### logosctl daemon stop

Stop the running daemon via RPC.

```
logosctl daemon stop
```

**Behavior:**
1. Refuses up front if `daemon/state.json` names this client's instance and a pid that is no longer alive (`NO_DAEMON`, exit 2). This is `ensureConnected()`'s guard, shared by every RPC-opening command, but `stop` is the one that would otherwise turn the silence into a *success*: steps 6-7 below read a missing reply plus a dead pid as a clean shutdown, and against a session that was already stale both are true before the command says anything
2. Connects to daemon via `Client`
3. Reads `daemon/state.json` for the daemon's pid **before** issuing the call — a clean shutdown deletes that file, so afterwards it is unreadable
4. Calls `core_service.shutdown()` (5s deadline; the daemon answers before doing any work, so a slower reply is a lost one)
5. core_service schedules a delayed shutdown request, `LOGOSCTL_SHUTDOWN_GRACE_MS` (default 200ms) later, that lets the provider queue the reply and then wakes the daemon
6. If the RPC response arrives: prints success and exits
7. If it does not, the client asks whether the daemon actually died, for up to 15s: by watching the pid from step 3, or — for a remote daemon, where there is no local pid — by re-probing `getStatus`. Gone ⇒ success, with `confirmed_by` naming the evidence. Still there ⇒ `RPC_FAILED`

**Exit codes:** 0 on success (including when daemon exits before response), 2 if no daemon (or a stale session), 3 if the daemon neither replied nor exited.

### logosctl info

Alias for `module-info`. Delegates to `module-info` command.

```
logosctl module show <module>
```

**Behavior:** Same as `module-info <module>` — see above.

**Exit codes:** 0 on success, 2 if no daemon, 3 if module not found.

---

## Call Chain: CLI → core_service → liblogos

```
logosctl command
  → RpcClient / PlainRpcClient
    → lp_client_invoke(core_service, method, JSON args)
      → qt_remote_plain wire
        → lp_provider callback
          → CoreServiceImpl::callMethodStd()
            → liblogos C API, or
            → PlainRpcClient for a target module
```

The last hop interoperates with current Qt module hosts because
`qt_remote_plain` reproduces the Qt Remote Objects framing, registry,
authentication, method metadata, calls, replies, and events without linking
Qt.

| CLI operation | core_service method | implementation |
|---|---|---|
| load/unload/reload/list/status/stats | matching method | liblogos C API |
| call | `callModuleMethod` | `lp_client_invoke` to the target module |
| watch | `watchModuleEvents` | `lp_subscribe`, forwarded as `module_event` |
| stop | `shutdown` | delayed condition-variable wakeup and cleanup |

## Build

### Nix

```bash
nix build

# The logosctl binary is at:
./result/bin/logosctl

# Run daemon
./result/bin/logosctl daemon start -m /path/to/modules
```

---

## Examples

### Basic Usage

```bash
# Start the daemon with module directories
logosctl daemon start --detach &

# Check daemon health
logosctl daemon status

# Load modules
logosctl module load waku
logosctl module load chat

# List loaded modules (with status and uptime)
logosctl module ls --loaded

# Get detailed module info
logosctl module show chat

# Call a method
logosctl call chat send_message "hello world"

# Reload a crashed module
logosctl module reload chat

# Watch events
logosctl watch chat --event chat-message

# Get stats
logosctl stats

# Stop daemon
logosctl daemon stop
```

### Agent / Script Usage

```bash
# Start daemon
logosctl daemon start --detach &
sleep 2

# Preflight: verify daemon is running
logosctl daemon status --json | jq -e '.daemon.status == "running"' > /dev/null

# Check what's available and their state
logosctl module ls --json
# [
#   {"name":"waku","version":"0.1.0","status":"not_loaded"},
#   {"name":"chat","version":"0.2.0","status":"not_loaded"}
# ]

# Load modules (JSON output for parsing)
logosctl module load waku --json
# {"status":"ok","module":"waku","version":"0.1.0","dependencies_loaded":["store"]}

logosctl module load chat --json
# {"status":"ok","module":"chat","version":"0.2.0","dependencies_loaded":[]}

# Discover methods before calling
logosctl module show chat --json | jq '.methods[].name'
# "send_message"
# "get_history"
# "get_status"

# Call method and parse result
RESULT=$(logosctl call chat send_message "hello" --json)
echo "$RESULT" | jq -r '.result'

# Handle crashed modules
MODULE_STATUS=$(logosctl daemon status --json | jq -r '.modules[] | select(.name=="chat") | .status')
if [ "$MODULE_STATUS" = "crashed" ]; then
  logosctl module show chat --json | jq '{exit_code, crash_signal, restart_count}'
  logosctl module reload chat --json
fi

# Stream events to log file
logosctl watch chat --event chat-message --json >> events.log &
WATCH_PID=$!

# Check overall health before cleanup
logosctl daemon status --json | jq '.modules_summary'
# {"loaded": 3, "crashed": 0, "not_loaded": 0}

# Cleanup
kill $WATCH_PID
logosctl daemon stop
```

### Using Environment Variables for Auth

```bash
# Set token via environment
export LOGOSCTL_TOKEN=xyz123

# Or inline per-command
LOGOSCTL_TOKEN=xyz123 logosctl module load waku

# Or via another local client/ tree (point client/config.json's token_file at a
# JSON file the daemon emitted). The file is a
# {"version":1,"name":"alice","token":"<raw>","issued_at":"<iso>"}
# object that `issue-token --name alice` writes to
# <session>/.logosctl/daemon/tokens/alice.json. Copy it into the other
# local session and reference it from
# client/config.json's token_file:
mkdir -p ~/.logosctl/client
cp ~/.logosctl/daemon/tokens/alice.json /path/to/session/client/
# then ensure ~/.logosctl/client/config.json's token_file = "alice.json"
logosctl module load waku
```

### Piping and Composition

```bash
# Filter loaded modules
logosctl module ls --json | jq '[.[] | select(.status == "loaded")]'

# Find crashed modules
logosctl module ls --json | jq '[.[] | select(.status == "crashed")]'

# Watch events and filter
logosctl watch chat --event chat-message --json | jq 'select(.data.from == "alice")'

# Monitor module health with status dashboard
watch -n 5 'logosctl daemon status --json | jq "{daemon: .daemon.status, modules: .modules_summary}"'

# Monitor resource usage
watch -n 5 'logosctl stats --json | jq ".[] | {name, cpu_percent, memory_mb}"'

# Auto-reload crashed modules
logosctl module ls --json | jq -r '.[] | select(.status == "crashed") | .name' | while read mod; do
  logosctl module reload "$mod" --json
done
```

---

## Tests

| Test File | Coverage |
|-----------|----------|
| `test_commands.cpp` | All subcommand implementations via mock client: load/unload/reload module, list-modules, status, module-info, call, stats, watch, stop. Tests both success and error paths, JSON and human output modes. |
| `test_mode_detection.cpp` | Mode detection (daemon/client/help/version), known subcommands list, argument parsing. |
| `test_output.cpp` | Output formatter (human/JSON), TTY detection, printSuccess/printError/printRaw. |
| `test_daemon_state.cpp` | Round-trip `daemon/state.json` — instance_id, pid, modulesDirs, per-module `transports` entries (local/tcp/tcp_ssl, codec defaulting), and the `tokens` array (`name, hash, issued_at, expires_at, local_only`). `fileOk` is independent of the pid (it's a parse check, not liveness). |
| `test_token_store.cpp` | Token issuance (including `--expires` and `--local-only`), duplicate-name rejection (unless `--replace`), revocation, list, persistence round-trip. Confirms `tokens.json["tokens"]` stores hashes only; plaintext lives in `daemon/tokens/<name>.json`. Fail-closed invariants: an empty token never authenticates, `issueToken` Ok implies a non-empty token, a failed `--replace` preserves the prior raw token, and issuing against an unsupported-schema-version file refuses instead of clobbering it. |
| `test_config.cpp` | Token resolution order (env var → `client/<token_file>`); `client/config.json` parsing; `clientTokenPath` accepts plain filenames and rejects path-traversal (`../`, absolute, sub-dirs). |
| `test_port_allocator.cpp` | Ephemeral-port allocation: bad host returns 0, an IPv6 any-address (`::`) allocates a port, consecutive allocations are distinct. |
| `test_access_policy_arg.cpp` | `--access-policy` resolution: the `enforce` alias expands to the deny-by-default document, the alias beats the file branch, inline JSON and file paths pass through unchanged, and a bad path / malformed JSON fails with a reason rather than degrading to "no policy". |
| `test_log_sink.cpp` | Pipe-based stdout/stderr capture into the rotating daemon log. |
| `test_paths.cpp` | Executable / bundle-relative path resolution (`paths.h`). |
| `test_cli.cpp` | End-to-end CLI tests: help, version, no-args, client commands without daemon, daemon startup with --verbose; rejection of an invalid `--module-transport` port, an invalid `--client-codec`, and a `--token-file` that carries no usable token. |
| `test_integration.cpp` | Daemon-backed integration: a real `logosctl` daemon against a real module directory, driven through the client subcommands — error paths, the full `test_basic_module` API surface, event subscription via `watch`, and many simultaneous clients on one daemon. |
| `test_cli_logoscore.cpp` / `test_integration_logoscore.cpp` | The same two suites frozen against `logoscore`'s surface, so shared-runtime changes can't regress the tool people actually use. They get deleted with the binary. |

`checks.<sys>.tests` runs the unit and CLI suites. Both `integration_tests` suites need real
module plugins, so they run in logos-test-modules as `checks.<sys>.logoscore-cli-integration-*`.

---

## Known Issues

1. Event forwarding adds one relay hop through `core_service`.
2. Local endpoint probing deliberately fails closed on ambiguous filesystem or
   platform errors, so those cases wait for the normal RPC timeout.

## Future Improvements

1. Add token scopes and per-module authorization policy.
2. Allow clients to subscribe directly when a secure transport-discovery
   mechanism exists, avoiding the core-service relay hop.
