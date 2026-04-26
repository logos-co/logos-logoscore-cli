# Logoscore CLI Specification

## Overview

The logoscore CLI is the primary interface for operating the Logos Core runtime. It manages the lifecycle of a daemon process that hosts independently developed modules (plugins), and provides commands to load modules, call methods, watch events, and inspect runtime state.

The CLI follows a daemon + client architecture. A long-running daemon process hosts the module runtime, and short-lived client commands connect to it to perform operations.

### Design Goals

1. **Human-friendly** — Readable output, discoverable commands, helpful error messages with recovery suggestions.
2. **Agent-friendly** — Structured JSON output, non-interactive operation, streaming events as NDJSON, deterministic exit codes. An AI agent using a bash tool should be able to operate the full lifecycle without any interactive prompts or ambiguous output.
3. **Composable** — Each command does one thing and works well in pipelines. Output goes to stdout, diagnostics to stderr.
4. **Backward-compatible** — The existing flat-flag interface (`-m`, `-l`, `-c`, `--quit-on-finish`) continues to work.

---

## Architecture

```
                    ┌──────────────────────┐
                    │   logoscore daemon   │
                    │                      │
                    │  ┌────────────────┐  │
                    │  │  Core Manager  │  │
                    │  │  (RPC server)  │  │
                    │  └───────▲────────┘  │
                    │          │           │
                    │   Qt Remote Objects  │
                    │          │           │
                    └──────────┼───────────┘
                               │
              ┌────────────────┼────────────────┐
              │                │                │
     ┌────────▼──────┐ ┌──────▼───────┐ ┌──────▼───────┐
     │ logoscore     │ │ logoscore    │ │ logoscore    │
     │ load-module   │ │ call chat   │ │ watch chat   │
     │ waku          │ │ send "hi"   │ │ --event msg  │
     └───────────────┘ └──────────────┘ └──────────────┘
         (exits)          (exits)        (streams)
```

**Daemon** (`logoscore -D`):
- Starts the Logos Core runtime and Qt event loop.
- Discovers modules in configured directories.
- Writes a connection file to `~/.logoscore/daemon.json` containing the registry URL and a client token.
- Removes the connection file on shutdown.

**Client commands** (all other subcommands):
- Read `~/.logoscore/daemon.json` to locate the running daemon and obtain the client token.
- Connect via the Core Manager's RPC interface using the token.
- Execute the requested operation, print the result, and exit.
- If no daemon is running, exit with code 2 and a clear error message.

---

## Command Structure

```
logoscore [global-flags] <command> [command-flags] [args...]
```

### Global Flags

| Flag | Short | Description |
|------|-------|-------------|
| `--json` | `-j` | Output as JSON. Default when stdout is not a TTY. |
| `--modules-dir <path>` | `-m` | Module search directory (daemon mode only, repeatable). |
| `--config-dir <path>` | | Override the config directory (default: `~/.logoscore`; also `LOGOSCORE_CONFIG_DIR`). Client commands must pass the same value as the daemon they target. |
| `--quiet` | `-q` | Suppress non-essential output. |
| `--verbose` | `-v` | Show debug/info/warning logs (suppressed by default). |
| `--help` | `-h` | Show help. |
| `--version` | | Show version. |

#### Daemon-side transport flags

The daemon defaults to a local Unix socket only. To expose `core_service` over
the network, pass one or more `--transport=<name>` flags; each opens an
additional listener that gets advertised in `daemon.json`.

| Flag | Applies to | Description |
|------|------------|-------------|
| `--transport <name>` | daemon | Repeatable. One of `local`, `tcp`, `tcp_ssl`. Each occurrence opens a listener. If omitted entirely, `local` is implied. |
| `--tcp-host <host>` | daemon | TCP bind address (default `127.0.0.1`). Use `0.0.0.0` to accept remote connections. |
| `--tcp-port <port>` | daemon | TCP port (`0` = auto-assign). |
| `--tcp-codec <codec>` | daemon | Wire codec for `tcp`: `json` (default, debuggable) or `cbor` (compact). |
| `--tcp-ssl-host <host>` | daemon | `tcp_ssl` bind address. |
| `--tcp-ssl-port <port>` | daemon | `tcp_ssl` port (`0` = auto-assign). |
| `--tcp-ssl-codec <codec>` | daemon | Wire codec for `tcp_ssl`. |
| `--ssl-ca <path>` | daemon | CA cert PEM file (`tcp_ssl`). |
| `--ssl-cert <path>` | daemon | Server cert PEM file (`tcp_ssl`). |
| `--ssl-key <path>` | daemon | Server private key PEM file (`tcp_ssl`). |

#### Client-side transport flags

Client commands pick which advertised transport to dial and may override the
host/port — needed when the address the daemon bound to differs from the
reachable one (port-forwarded containers, NAT, SSH tunnels). If none are
passed, the client prefers `local`; if the daemon didn't advertise `local`
it errors out rather than guessing.

| Flag | Applies to | Description |
|------|------------|-------------|
| `--client-transport <name>` | client | Which advertised transport to use: `local`, `tcp`, `tcp_ssl`. |
| `--client-tcp-host <host>` | client | Dial this host instead of the advertised one. |
| `--client-tcp-port <port>` | client | Dial this port instead of the advertised one. |
| `--client-codec <codec>` | client | Require this codec; aborts if it doesn't match what the daemon advertised for the chosen transport (prevents mixed-codec corruption). |
| `--no-verify-peer` | client | Skip TLS peer verification. Dev only. |

The client-side overrides also propagate via `LOGOSCORE_CLIENT_*` env vars, so
subprocesses (e.g. the Python wrapper, or `logoscore watch` that spawns a
watcher) inherit them.

---

## Commands

### `daemon` / `-D`

Start the daemon process.

```
logoscore -D [--modules-dir <path>]...
logoscore daemon [--modules-dir <path>]...
```

Starts the Logos Core runtime in the foreground. Startup and shutdown messages go to stdout (so `> logs.txt` captures them); debug/info/warning logs go to stderr and are suppressed unless `--verbose` is passed. Writes `~/.logoscore/daemon.json` on startup, removes it on clean shutdown.

The daemon scans the configured module directories for available plugins and makes them available for loading via client commands.

### `load-module <name>`

Load a module into the running daemon.

```
logoscore load-module <name>
```

Resolves and loads the named module and all its dependencies. The module must be discoverable in one of the directories configured when the daemon was started.

### `unload-module <name>`

Unload a module from the running daemon.

```
logoscore unload-module <name>
```

### `list-modules`

List available or loaded modules.

```
logoscore list-modules [--loaded]
```

Without flags, lists all known (discovered) modules. With `--loaded`, lists only currently loaded modules.

Each module has a status: `loaded`, `not_loaded`, `crashed`, or `loading`. When a module has crashed, the output includes uptime (or `-` if not running) and crash metadata is available via `module-info`.

### `status`

Show overall daemon and module health.

```
logoscore status
```

Displays daemon state (PID, uptime, version, instance ID, socket path) and a summary of all modules with their status. This is the single "dashboard" command — it shows everything at a glance so agents don't need to chain multiple commands.

When the daemon is not running, exits with code 2 and suggests how to start it.

### `reload-module <name>`

Unload and re-load a module.

```
logoscore reload-module <name>
```

Performs an unload followed by a load in a single operation. Useful for recovering crashed modules or picking up configuration changes. If the module is not currently loaded, it falls back to a plain load (rather than erroring), reducing edge cases for agents that just want a module running.

### `module-info <name>`

Show detailed information about a specific module.

```
logoscore module-info <name>
```

Displays extended metadata: version, status, dependencies, available methods, process info (PID, uptime), and crash details if applicable. This is the deep-inspection counterpart to `list-modules`.

### `call <module> <method> [args...]`

Call a method on a loaded module.

```
logoscore call <module> <method> [args...]
```

Invokes the named method on the specified module. Arguments are positional. Use the `@file` prefix to read a parameter value from a file.

Arguments are automatically type-coerced: numeric strings become integers or doubles, `"true"`/`"false"` become booleans, and everything else remains a string. This allows method signatures with typed parameters to match correctly.

```bash
logoscore call chat send_message "hello"
logoscore call storage load_config @config.json
logoscore call math twoArgs "hello" 2          # "hello" as string, 2 as integer
logoscore call config setBool "flag" true       # "flag" as string, true as boolean
```

**Alternative syntax** (explicit form for readability):

```bash
logoscore module <name> method <method> [args...]
logoscore module chat method send_message "hello"
```

Both forms are equivalent. `call` is the short form; `module ... method ...` is the verbose form.

### `watch <module> [--event <name>]`

Watch events from a loaded module.

```
logoscore watch <module> [--event <name>]
```

Streams events to stdout as they arrive. Without `--event`, streams all events from the module. Runs until interrupted (SIGINT / SIGTERM).

```bash
logoscore watch chat --event chat-message
logoscore watch chat --event chat-message >> events.log &
logoscore watch chat --event chat-message --json | jq .
```

### `stats`

Show resource usage for loaded modules.

```
logoscore stats
```

Displays CPU and memory usage for each loaded module process.

### `stop`

Stop the running daemon.

```
logoscore stop
```

Sends a shutdown request to the daemon via `core_service`. The daemon performs a clean shutdown: unloads all modules, removes the connection file, and exits. The client prints a confirmation message and exits.

If the daemon exits before the RPC response arrives (expected behavior), the client treats the connection loss as a successful shutdown.

**Human:**
```
$ logoscore stop
Daemon stopped.
```

**JSON:**
```json
$ logoscore stop --json
{"status":"ok","message":"Daemon shutting down."}
```

### `info <module>`

Alias for `module-info <module>`. See `module-info` above for full details.

```
logoscore info <module>
```

Displays version, dependencies, available methods, and crash details (if applicable) for the named module.

### `issue-token --name <name>`

Issue a new named token and write it to `<configDir>/tokens/<name>.json`.

```
logoscore issue-token --name <name> [--replace]
```

Writes an entry to `<configDir>/tokens.db` and a companion `tokens/<name>.json`
for distribution. Without `--replace`, the command refuses to overwrite an
existing token with the same name so a stale credential isn't silently
invalidated; pass `--replace` to rotate.

The client file carries the raw token and enough endpoint metadata for a
remote client to connect. Distribute it the way you'd distribute a private
key — do not commit it to version control.

This command operates directly on the config dir on disk; it doesn't need the
daemon to be running. Useful for pre-provisioning credentials before startup.

### `revoke-token <name>`

Remove a named token from `<configDir>/tokens.db`.

```
logoscore revoke-token <name>
```

After this returns, any RPC presenting the revoked token is rejected by the
daemon with an authentication error. The on-disk `tokens/<name>.json` file is
also removed so clients that still have it can't mistake it for valid.

### `list-tokens`

List all tokens currently issued against this config dir.

```
logoscore list-tokens
```

Shows the token name and issued-at timestamp only — never the plaintext token,
which only lives in the `tokens/<name>.json` file at the moment of issuance.
Lost a token? Rotate it with `issue-token --replace`.

---

## Authentication

### How Tokens Work

Logos Core uses UUID-based tokens for authentication. Every module loaded into the runtime receives a unique token generated by the core. These tokens are used to authorize RPC calls between components.

The CLI needs a token to authenticate with the daemon's Core Manager. This token is called the **client token** and is generated by the daemon on startup.

### Token Lifecycle

```
1. DAEMON STARTS
   logoscore -D -m ./modules
   → Core generates a client token (UUID)
   → Token written to ~/.logoscore/daemon.json
   → Token saved in core's TokenManager under key "cli_client"

2. CLIENT CONNECTS
   logoscore load-module waku
   → Reads token from ~/.logoscore/daemon.json
   → Sends token with RPC request to Core Manager
   → Core Manager validates token against TokenManager
   → Request authorized, module loads

3. REMOTE / PROGRAMMATIC ACCESS
   LOGOSCORE_TOKEN=<token> logoscore load-module waku
   → Token from env var overrides the one in daemon.json
   → Useful when daemon.json is not accessible (remote, containers, CI)
```

### Token Resolution Order

When a client command runs, the token is resolved in this order (first match wins):

| Priority | Source | Example |
|----------|--------|---------|
| 1 | `LOGOSCORE_TOKEN` env var | `LOGOSCORE_TOKEN=abc123 logoscore list-modules` |
| 2 | Config file | `<configDir>/config.json` → `{"token": "abc123"}` |
| 3 | Connection file | `<configDir>/daemon.json` → `{"token": "abc123"}` |

A named token issued by `logoscore issue-token --name alice` lives in
`<configDir>/tokens/alice.json`. To use it as a client on a different machine,
copy the raw token out of that file into `LOGOSCORE_TOKEN` or into the remote
host's `config.json`. The per-client file also records endpoint metadata, so
tooling can read both the token and the reachable endpoint from one place.

### Obtaining a Token

**Local usage (same machine):** No manual token management needed. The daemon writes its token to `~/.logoscore/daemon.json` and client commands read it automatically. This is the default workflow.

**Remote or programmatic usage:** Extract the token from the daemon's connection file and pass it to the remote client:

```bash
# On the machine running the daemon:
cat ~/.logoscore/daemon.json | jq -r '.token'
# Output: 550e8400-e29b-41d4-a716-446655440000

# On the remote machine or in a script:
export LOGOSCORE_TOKEN=550e8400-e29b-41d4-a716-446655440000
logoscore list-modules --json

# Or persist in config:
mkdir -p ~/.logoscore
echo '{"token": "550e8400-e29b-41d4-a716-446655440000"}' > ~/.logoscore/config.json
```

**CI / containers:** Pass the token as an environment variable at runtime:

```bash
docker run -e LOGOSCORE_TOKEN=$TOKEN myimage logoscore list-modules --json
```

### Connection File

The daemon writes `<configDir>/daemon.json` on startup:

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

- `instance_id` is a 12-char UUID prefix the client uses with `LogosInstance::id()`
  to reconstruct the same registry URL the daemon published (`local:logos_core_service_<id>`).
- `transports` is the list of endpoints the daemon opened. Empty / absent
  means the old-style default of one local-socket transport. Clients pick one
  based on `--client-transport` (or `local` by default if offered).
- `token` is the automatically-issued CLI client token. Named tokens
  (`issue-token`) live in `tokens.db` + `tokens/<name>.json` and aren't
  advertised here.

The client uses this file to answer *how* to reach the daemon. Liveness — *is
the daemon actually answering?* — is no longer pre-probed; the first RPC (e.g.
`status`) surfaces a connect failure via the same code path as any other
method call, so there's one error story instead of "probe ok, RPC fails".

The daemon removes this file on clean shutdown.

---

## Output Design

Every command produces output in one of two modes: human (default when stdout is a TTY) or JSON (when `--json` is passed or stdout is piped/redirected).

### `load-module`

**Human:**
```
$ logoscore load-module waku
Loaded module: waku (v0.1.0)
  Dependencies loaded: store
```

**JSON:**
```
$ logoscore load-module waku --json
{"status":"ok","module":"waku","version":"0.1.0","dependencies_loaded":["store"]}
```

**Error (human):**
```
$ logoscore load-module nonexistent
Error: Module 'nonexistent' not found.
  Known modules: waku, chat, delivery, store
  Scan additional directories with: logoscore -D -m /path/to/modules
```

**Error (JSON):**
```
$ logoscore load-module nonexistent --json
{"status":"error","code":"MODULE_NOT_FOUND","message":"Module 'nonexistent' not found.","known_modules":["waku","chat","delivery","store"]}
```

### `unload-module`

**Human:**
```
$ logoscore unload-module waku
Unloaded module: waku
```

**JSON:**
```
$ logoscore unload-module waku --json
{"status":"ok","module":"waku"}
```

### `list-modules`

**Human:**
```
$ logoscore list-modules
NAME        VERSION   STATUS      UPTIME
waku        v0.1.0    loaded      2h 14m
chat        v0.2.0    crashed     -
delivery    v0.1.0    not loaded  -
store       v0.3.0    loaded      2h 14m

$ logoscore list-modules --loaded
NAME        VERSION   STATUS    UPTIME
waku        v0.1.0    loaded    2h 14m
store       v0.3.0    loaded    2h 14m
```

**JSON:**
```json
$ logoscore list-modules --json
[
  {"name":"waku","version":"0.1.0","status":"loaded","uptime_seconds":8040},
  {"name":"chat","version":"0.2.0","status":"crashed","exit_code":139,"crashed_at":"2026-03-23T14:22:01Z","crash_reason":"SIGSEGV"},
  {"name":"delivery","version":"0.1.0","status":"not_loaded"},
  {"name":"store","version":"0.3.0","status":"loaded","uptime_seconds":8040}
]
```

Note: the `status` field is an enum of `loaded | not_loaded | crashed | loading`. Crash metadata (`exit_code`, `crashed_at`, `crash_reason`) only appears when status is `crashed` — the JSON doesn't bloat clean entries with null crash fields.

### `status`

**Human:**
```
$ logoscore status
Logoscore Daemon
  Status:       running
  PID:          12847
  Uptime:       4h 32m
  Version:      v0.5.0
  Instance ID:  a3f1...c8d2
  Socket:       /Users/iuri/.logoscore/daemon.sock

Modules: 3 loaded, 1 crashed, 1 not loaded
  waku        v0.1.0    loaded      2h 14m
  chat        v0.2.0    crashed     -
  delivery    v0.1.0    not loaded  -
  store       v0.3.0    loaded      4h 32m
  payments    v0.1.0    loaded      4h 32m
```

**JSON:**
```json
$ logoscore status --json
{
  "daemon": {
    "status": "running",
    "pid": 12847,
    "uptime_seconds": 16320,
    "version": "0.5.0",
    "instance_id": "a3f1...c8d2",
    "socket": "/Users/iuri/.logoscore/daemon.sock"
  },
  "modules_summary": {
    "loaded": 3,
    "crashed": 1,
    "not_loaded": 1
  },
  "modules": [
    {"name":"waku","version":"0.1.0","status":"loaded","uptime_seconds":8040},
    {"name":"chat","version":"0.2.0","status":"crashed","exit_code":139,"crashed_at":"2026-03-23T14:22:01Z"},
    {"name":"delivery","version":"0.1.0","status":"not_loaded"},
    {"name":"store","version":"0.3.0","status":"loaded","uptime_seconds":16320},
    {"name":"payments","version":"0.1.0","status":"loaded","uptime_seconds":16320}
  ]
}
```

**When daemon is not running:**
```
$ logoscore status
Logoscore Daemon
  Status:       not running

No socket found at /Users/iuri/.logoscore/daemon.sock
Run "logoscore -D" to start the daemon.

$ echo $?
1
```

```json
$ logoscore status --json
{
  "daemon": {
    "status": "not_running"
  }
}
$ echo $?
1
```

### `reload-module`

**Human:**
```
$ logoscore reload-module chat
Unloading chat...  done
Loading chat...    done
Module "chat" reloaded successfully (v0.2.0, pid 51203)
```

**JSON:**
```json
$ logoscore reload-module chat --json
{
  "action": "reload",
  "module": "chat",
  "version": "0.2.0",
  "status": "loaded",
  "pid": 51203,
  "previous_status": "crashed",
  "duration_ms": 340
}
```

**When reload fails:**
```
$ logoscore reload-module chat
Unloading chat...  done
Loading chat...    failed

Error: module "chat" failed to start (exit code 1)
  Last log: "Config file not found: /etc/logoscore/chat.toml"

Run "logoscore module-logs chat --tail 20" for details.

$ echo $?
3
```

```json
$ logoscore reload-module chat --json
{
  "action": "reload",
  "module": "chat",
  "status": "error",
  "error": "module failed to start",
  "exit_code": 1,
  "last_log_line": "Config file not found: /etc/logoscore/chat.toml"
}
$ echo $?
3
```

**Reload a module that isn't loaded (behaves like load):**
```
$ logoscore reload-module delivery
Module "delivery" is not loaded. Loading...
Loading delivery...  done
Module "delivery" loaded successfully (v0.1.0, pid 51210)
```

### `module-info`

**Human:**
```
$ logoscore module-info chat
Name:          chat
Version:       v0.2.0
Status:        loaded
PID:           23457
Uptime:        2h 14m
Dependencies:  waku, store

Methods:
  send_message(text: QString) -> QString
  get_history() -> QJsonArray
  set_nickname(name: QString) -> bool
  get_status() -> QString
```

**Crashed module:**
```
$ logoscore module-info chat
Name:          chat
Version:       v0.2.0
Status:        crashed
Exit Code:     139 (SIGSEGV)
Crashed At:    2026-03-23T14:22:01Z
Restart Count: 3
Last Log:      "Segmentation fault in message_handler.cpp:142"
```

**JSON:**
```json
$ logoscore module-info chat --json
{
  "name": "chat",
  "version": "0.2.0",
  "status": "loaded",
  "pid": 23457,
  "uptime_seconds": 8040,
  "dependencies": ["waku", "store"],
  "methods": [
    {"name": "send_message", "params": [{"name": "text", "type": "QString"}], "return_type": "QString"},
    {"name": "get_history", "params": [], "return_type": "QJsonArray"},
    {"name": "set_nickname", "params": [{"name": "name", "type": "QString"}], "return_type": "bool"},
    {"name": "get_status", "params": [], "return_type": "QString"}
  ]
}
```

**Crashed module (JSON):**
```json
$ logoscore module-info chat --json
{
  "name": "chat",
  "version": "0.2.0",
  "status": "crashed",
  "exit_code": 139,
  "crash_signal": "SIGSEGV",
  "crashed_at": "2026-03-23T14:22:01Z",
  "restart_count": 3,
  "last_log_line": "Segmentation fault in message_handler.cpp:142",
  "pid_before_crash": 48291
}
```

### `call`

**Human:**
```
$ logoscore call chat send_message "hello world"
message sent (id: msg_4a7b2c)

$ logoscore call math add 2 3
5
```

In human mode, scalar results (strings, numbers, booleans) are printed as plain values. Structured results (objects, arrays) are printed as indented JSON. Null results produce no output.

**JSON:**
```
$ logoscore call chat send_message "hello world" --json
{"status":"ok","module":"chat","method":"send_message","result":"message sent (id: msg_4a7b2c)"}
```

When the method returns structured data:
```
$ logoscore call chat get_history --json
{"status":"ok","module":"chat","method":"get_history","result":[{"id":"msg_4a7b2c","from":"alice","text":"hello","timestamp":"2026-03-23T14:30:01Z"},{"id":"msg_5d8e3f","from":"bob","text":"hi there","timestamp":"2026-03-23T14:30:05Z"}]}
```

**LogosResult return values:**

Methods declared to return `LogosResult` (the common ok/error wrapper) are
serialised as:

```json
{"success": <bool>, "value": <any>, "error": <any>}
```

`value` is whatever the method stuffed in on success; `error` is whatever it
stuffed in on failure; the unused side is `null`. Same shape regardless of
whether the daemon-module hop went over the local socket (QRO), TCP, or
TCP+SSL — pick the transport you like, assertions stay identical.

```
$ logoscore call account create_account --json
{"status":"ok","module":"account","method":"create_account",
 "result":{"success":true,"value":{"id":"42","name":"alice"},"error":null}}

$ logoscore call account create_account --json    # duplicate name
{"status":"ok","module":"account","method":"create_account",
 "result":{"success":false,"value":null,"error":"name already taken"}}
```

**Error (human):**
```
$ logoscore call chat nonexistent_method
Error: Method 'nonexistent_method' not found on module 'chat'.
  Available methods: send_message, get_history, set_nickname, get_status
```

**Error (JSON):**
```
$ logoscore call chat nonexistent_method --json
{"status":"error","code":"METHOD_NOT_FOUND","message":"Method 'nonexistent_method' not found on module 'chat'.","available_methods":["send_message","get_history","set_nickname","get_status"]}
```

**Timeout error (JSON):**
```
$ logoscore call chat slow_operation --json
{"status":"error","code":"TIMEOUT","message":"Call to chat.slow_operation timed out after 30s."}
```

### `watch`

Streams continuously until interrupted. Each event is printed as it arrives.

**Human:**
```
$ logoscore watch chat --event chat-message
[14:30:01] chat :: chat-message
  from: alice
  text: hello world

[14:30:05] chat :: chat-message
  from: bob
  text: hi there

[14:31:12] chat :: chat-message
  from: alice
  text: how are you?
^C
```

**JSON (NDJSON — one self-contained JSON object per line):**
```
$ logoscore watch chat --event chat-message --json
{"timestamp":"2026-03-23T14:30:01Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"hello world"}}
{"timestamp":"2026-03-23T14:30:05Z","module":"chat","event":"chat-message","data":{"from":"bob","text":"hi there"}}
{"timestamp":"2026-03-23T14:31:12Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"how are you?"}}
```

All events from a module (no `--event` filter):
```
$ logoscore watch chat --json
{"timestamp":"2026-03-23T14:30:01Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"hello"}}
{"timestamp":"2026-03-23T14:30:02Z","module":"chat","event":"user-joined","data":{"user":"bob"}}
{"timestamp":"2026-03-23T14:30:05Z","module":"chat","event":"chat-message","data":{"from":"bob","text":"hi"}}
{"timestamp":"2026-03-23T14:30:06Z","module":"chat","event":"typing","data":{"user":"alice"}}
```

### `stats`

**Human:**
```
$ logoscore stats
MODULE      PID     CPU%    MEMORY
waku        23456   2.1%    48.3 MB
chat        23457   0.4%    22.1 MB
store       23458   0.1%    15.7 MB
```

**JSON:**
```
$ logoscore stats --json
[
  {"name":"waku","pid":23456,"cpu_percent":2.1,"memory_mb":48.3},
  {"name":"chat","pid":23457,"cpu_percent":0.4,"memory_mb":22.1},
  {"name":"store","pid":23458,"cpu_percent":0.1,"memory_mb":15.7}
]
```

### `info`

Alias for `module-info`. See the `module-info` output section above for all output examples including human, JSON, and crashed module variants.

### No daemon running

**Human:**
```
$ logoscore list-modules
Error: No running logoscore daemon.
  Start one with: logoscore -D
  Start with modules: logoscore -D -m /path/to/modules
```

**JSON:**
```
$ logoscore list-modules --json
{"status":"error","code":"NO_DAEMON","message":"No running logoscore daemon. Start one with: logoscore -D"}
```

### Output Rules

- Primary output (results, data) goes to stdout.
- Debug, info, and warning logs go to stderr and are suppressed by default. Pass `--verbose` to show them.
- Critical and fatal errors always go to stderr.
- In JSON mode, colors are disabled and only structured data goes to stdout.
- JSON mode auto-activates when stdout is not a TTY (piped or redirected), so agents and scripts get JSON by default without needing `--json`.
- Daemon startup/shutdown messages go to stdout, so `logoscore -D > logs.txt` captures them correctly.

---

## Error Handling

### Exit Codes

| Code | Meaning | When |
|------|---------|------|
| `0` | Success | Operation completed |
| `1` | General error | Unexpected failure, invalid arguments |
| `2` | Connection error | No daemon running, daemon unreachable |
| `3` | Module error | Module not found, failed to load/unload |
| `4` | Method error | Method not found, invocation failed, timeout |

### JSON Error Envelope

All errors in JSON mode follow this structure:

```json
{
  "status": "error",
  "code": "ERROR_CODE",
  "message": "Human-readable description with recovery suggestion."
}
```

Error codes: `NO_DAEMON`, `DAEMON_UNREACHABLE`, `MODULE_NOT_FOUND`, `MODULE_LOAD_FAILED`, `MODULE_NOT_LOADED`, `METHOD_NOT_FOUND`, `METHOD_FAILED`, `TIMEOUT`, `AUTH_FAILED`, `INVALID_ARGS`.

---

## Backward Compatibility

The existing flat-flag interface continues to work as inline mode (no daemon needed):

```bash
# Legacy mode — start core, load modules, call methods, exit
logoscore -m /path -l waku,chat -c "chat.send_message(hello)" --quit-on-finish

# Equivalent using new subcommands (requires running daemon)
logoscore -D -m /path &
logoscore load-module waku
logoscore load-module chat
logoscore call chat send_message "hello"
```

When legacy flags (`-l`, `-c`) are present, the binary operates in inline mode: it starts the core, performs the requested operations, and exits. When a subcommand is detected, it operates in client mode and connects to a running daemon.

---

## AI Agent Workflow

This section describes how an AI agent (such as Claude Code, Cursor, or similar tools that execute bash commands via a tool-use interface) would interact with the logoscore CLI.

### How Agents Use This CLI

AI agents interact with CLIs by executing bash commands and parsing stdout. They cannot handle interactive prompts, colored output, or ambiguous formatting. The logoscore CLI is designed for this:

- **JSON by default when piped.** Since agents capture stdout programmatically (not via a TTY), JSON mode activates automatically. No need to remember `--json`.
- **Deterministic exit codes.** Agents check `$?` after each command to decide whether to proceed or handle an error. Each error category has a distinct code.
- **Structured errors.** When something fails, the JSON error includes a `code` field the agent can branch on, and a `message` field with recovery instructions the agent can follow.
- **No interactive prompts.** Every operation completes without requiring user input.
- **Self-describing.** `logoscore info <module> --json` tells the agent what methods are available and what parameters they take, without needing external documentation.

### Example: Agent Preflight — Health Check Before Doing Work

Before performing any operation, an agent checks daemon health and ensures required modules are running:

```bash
# Step 1: Is the daemon alive?
if ! logoscore status --json | jq -e '.daemon.status == "running"' > /dev/null 2>&1; then
  echo "daemon not running, starting..."
  logoscore -D -m ./modules &
  sleep 2
fi

# Step 2: Check if the module I need is healthy
MODULE_STATUS=$(logoscore status --json | jq -r '.modules[] | select(.name=="chat") | .status')

case "$MODULE_STATUS" in
  "loaded")     echo "ready" ;;
  "crashed")    logoscore reload-module chat ;;
  "not_loaded") logoscore load-module chat ;;
  *)            echo "unknown state: $MODULE_STATUS" ; exit 1 ;;
esac
```

### Example: Agent Detects and Recovers a Crashed Module

```bash
# Agent checks module health
STATUS=$(logoscore list-modules --json | jq -r '.[] | select(.name=="chat") | .status')

if [ "$STATUS" = "crashed" ]; then
  # Get crash details for decision-making
  CRASH_INFO=$(logoscore module-info chat --json)
  RESTARTS=$(echo "$CRASH_INFO" | jq '.restart_count')

  if [ "$RESTARTS" -lt 5 ]; then
    logoscore reload-module chat
  else
    echo "chat module crashed $RESTARTS times, escalating"
    # agent decides to alert or investigate logs
    logoscore module-logs chat --tail 50
  fi
fi
```

### Example: Agent Builds and Tests a Chat Application

This is a realistic sequence an AI agent would execute when asked to "set up and test the chat module":

```bash
# Step 1: Start the daemon and verify it's running
logoscore -D -m ./modules &
sleep 2
logoscore status --json | jq -e '.daemon.status == "running"' > /dev/null
# Agent confirms daemon is up via exit code 0.

# Step 2: Check what modules are available
logoscore list-modules --json
# Agent parses:
# [
#   {"name":"waku","version":"0.1.0","status":"not_loaded"},
#   {"name":"chat","version":"0.2.0","status":"not_loaded"},
#   {"name":"store","version":"0.3.0","status":"not_loaded"}
# ]
# Agent reads the array and identifies "chat" is available.

# Step 3: Load the chat module
logoscore load-module chat
# Agent parses:
# {"status":"ok","module":"chat","version":"0.2.0","dependencies_loaded":["waku","store"]}
# Agent confirms status is "ok" and notes that waku and store were auto-loaded.

# Step 4: Discover what methods are available
logoscore module-info chat --json
# Agent parses:
# {
#   "name": "chat",
#   "version": "0.2.0",
#   "status": "loaded",
#   "pid": 23457,
#   "uptime_seconds": 5,
#   "dependencies": ["waku", "store"],
#   "methods": [
#     {"name": "send_message", "params": [{"name": "text", "type": "QString"}], "return_type": "QString"},
#     {"name": "get_history", "params": [], "return_type": "QJsonArray"},
#     {"name": "get_status", "params": [], "return_type": "QString"}
#   ]
# }
# Agent now knows send_message takes a text param and get_history returns an array.

# Step 5: Call a method
logoscore call chat send_message "hello from agent"
# Agent parses:
# {"status":"ok","module":"chat","method":"send_message","result":"message sent (id: msg_9x8y7z)"}
# Agent confirms status is "ok".

# Step 6: Verify the message was stored
logoscore call chat get_history
# Agent parses:
# {"status":"ok","module":"chat","method":"get_history","result":[{"id":"msg_9x8y7z","from":"agent","text":"hello from agent","timestamp":"2026-03-23T14:30:01Z"}]}
# Agent verifies the message appears in history.

# Step 7: Check overall system health
logoscore status --json | jq '.modules_summary'
# Agent parses:
# {"loaded": 3, "crashed": 0, "not_loaded": 0}
# All modules healthy. Agent can also check per-module resource usage via `logoscore stats`.
```

### Example: Agent Handles Errors

When an agent encounters an error, the structured output lets it self-correct:

```bash
# Agent tries to call a method on a module that isn't loaded
logoscore call delivery send_package "pkg_123"
# Exit code: 3
# {"status":"error","code":"MODULE_NOT_LOADED","message":"Module 'delivery' is not loaded. Load it with: logoscore load-module delivery"}

# Agent reads the error code "MODULE_NOT_LOADED" and the recovery instruction.
# It follows the suggestion:
logoscore load-module delivery
# {"status":"ok","module":"delivery","version":"0.1.0","dependencies_loaded":[]}

# Now retries the original call:
logoscore call delivery send_package "pkg_123"
# {"status":"ok","module":"delivery","method":"send_package","result":"package pkg_123 queued"}
```

### Example: Agent Monitors Events

An agent can watch for events to react to real-time activity:

```bash
# Start watching in background, capture output to a file
logoscore watch chat --event chat-message > /tmp/chat_events.log &
WATCH_PID=$!

# ... agent does other work ...

# Later, check what events arrived
cat /tmp/chat_events.log
# {"timestamp":"2026-03-23T14:30:01Z","module":"chat","event":"chat-message","data":{"from":"alice","text":"hello"}}
# {"timestamp":"2026-03-23T14:30:05Z","module":"chat","event":"chat-message","data":{"from":"bob","text":"hi there"}}

# Agent can parse each line independently (NDJSON).
# Each line is valid JSON, so standard tools work:
# cat /tmp/chat_events.log | head -1 | jq '.data.from'
# → "alice"

# Cleanup
kill $WATCH_PID
```

### Why These Patterns Matter for Agents

| Pattern | Why it helps agents |
|---------|-------------------|
| JSON auto-detection (non-TTY) | Agent doesn't need to remember `--json` — it gets structured output automatically |
| Exit codes per error category | Agent can branch: `if exit_code == 2, start daemon; if exit_code == 3, load module` |
| Error messages with recovery commands | Agent can extract and execute the suggested fix directly |
| `status` as single dashboard | One command gives daemon health + all module states — no need to chain multiple commands |
| `module-info` with method signatures | Agent discovers available operations without documentation — it can read the schema and construct calls |
| `module-info` with crash metadata | Agent can programmatically distinguish OOM (137/SIGKILL) from segfault (139/SIGSEGV) from clean error (non-zero) |
| `reload-module` on unloaded module | Falls back to load instead of erroring — reduces edge cases for agents that just want a module running |
| NDJSON streaming | Agent processes events line-by-line without buffering the full stream |
| No interactive prompts | Agent never hangs waiting for input it can't provide |
| Consistent JSON envelope (`status`, `code`) | Agent uses the same parsing logic for all commands |

---

## Sequence Flows

### Starting the Daemon and Loading Modules

```
1. START DAEMON
   logoscore -D -m /path/to/modules
   → Core initializes
   → Generates client token (UUID)
   → Scans /path/to/modules for available plugins
   → Writes ~/.logoscore/daemon.json (with token)
   → Runs event loop (foreground)

2. LOAD MODULES
   logoscore load-module waku
   → Reads ~/.logoscore/daemon.json (gets registry URL + token)
   → Connects to daemon via Core Manager RPC with token
   → Daemon resolves dependencies for "waku"
   → Daemon loads dependencies first, then waku
   → Client prints result and exits

3. CALL METHODS
   logoscore call chat send_message "hello"
   → Reads token from daemon.json
   → Connects to daemon
   → Invokes chat.send_message("hello") via RPC
   → Prints return value to stdout
   → Exits

4. WATCH EVENTS
   logoscore watch chat --event chat-message --json >> events.log &
   → Connects to daemon with token
   → Registers event listener for chat::chat-message
   → Streams NDJSON to stdout (redirected to events.log)
   → Runs until killed

5. STOP DAEMON
   logoscore stop
   → Client sends shutdown RPC to core_service
   → Daemon schedules quit (with brief delay to send RPC response)
   → Daemon unloads all modules
   → Removes ~/.logoscore/daemon.json
   → Exits

   Alternatively: Ctrl+C / kill <pid> / SIGTERM
   → Signal handler triggers QCoreApplication::quit()
   → Same cleanup as above
```

