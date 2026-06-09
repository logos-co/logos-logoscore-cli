# Logoscore CLI Specification

## Overview

The logoscore CLI is the primary interface for operating the Logos Core runtime. It manages the lifecycle of a daemon process that hosts independently developed modules (plugins), and provides commands to load modules, call methods, watch events, and inspect runtime state.

The CLI follows a daemon + client architecture. A long-running daemon process hosts the module runtime, and short-lived client commands connect to it to perform operations.

### Design Goals

1. **Human-friendly** — Readable output, discoverable commands, helpful error messages with recovery suggestions.
2. **Agent-friendly** — Structured JSON output, non-interactive operation, streaming events as NDJSON, deterministic exit codes. An AI agent using a bash tool should be able to operate the full lifecycle without any interactive prompts or ambiguous output.
3. **Composable** — Each command does one thing and works well in pipelines. Output goes to stdout, diagnostics to stderr.
4. **Daemon-oriented** — A long-running daemon owns the modules; clients connect to it. `-m`/`-l`/`--persistence-path` configure daemon startup (with `-D`).

---

## Architecture

```
                    ┌──────────────────────────┐
                    │     logoscore daemon     │
                    │                          │
                    │  ┌────────────────────┐  │
                    │  │    core_service    │  │
                    │  │ (in-process module)│  │
                    │  └─────────▲──────────┘  │
                    │            │             │
                    │     Qt Remote Objects    │
                    │            │             │
                    └────────────┼─────────────┘
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
- Writes `~/.logoscore/daemon/state.json` (live runtime state — instance_id, pid, started_at, resolved transports) on startup, removed on clean shutdown.
- Maintains `~/.logoscore/daemon/tokens.json` (hashed-at-rest accepted-token list — survives restarts).
- Persists `~/.logoscore/daemon/config.json` (operator preferences) only when the operator passed `--persist-config`.
- Auto-issues an `auto` token for the local same-host client and emits `~/.logoscore/client/config.json` + `~/.logoscore/client/auto.json` on the first boot into an empty config dir.

**Client commands** (all other subcommands):
- Read `~/.logoscore/client/config.json` to learn how to dial the daemon and which token file to load.
- Connect to the daemon's `core_service` module via RPC using the token.
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
| `--config-dir <path>` | | Override the config directory (default: `~/.logoscore`; also `LOGOSCORE_CONFIG_DIR`). Client commands must pass the same value as the daemon they target. The directory contains the `daemon/` and `client/` subtrees (see [Authentication](#authentication)). |
| `--quiet` | `-q` | Suppress non-essential output. |
| `--verbose` | `-v` | Show debug/info/warning logs (suppressed by default). |
| `--help` | `-h` | Show help. |
| `--version` | | Show version. |

#### Daemon-side transport flags

The daemon defaults to a local Unix socket only for each well-known module
(`core_service`, `capability_module`). To expose either over the network,
pass one or more `--module-transport` flags; each opens an additional
listener that gets advertised in `daemon/state.json`'s `resolved` block.

**Local is always present.** Every module the operator configures (and
the two well-known ones) implicitly gets a LocalSocket listener
prepended to its resolved transport set, in addition to whatever the
operator named via `--module-transport`. The operator's TCP / TCP+SSL
flags add *additional* outside-facing surfaces; they don't replace the
same-host LocalSocket. This keeps the same-host code paths (the
parent's capability_module handshake, the SDK's auto-`requestModule`
flow inside `LogosAPIClient`, cross-module `getClient(name)` calls)
working over LocalSocket regardless of which network transport the
operator chose. `daemon/state.json`'s `resolved.modules.<name>.transports[]`
always lists the LocalSocket entry first, followed by operator-named
entries in the order they were typed.

| Flag | Applies to | Description |
|------|------------|-------------|
| `--module-transport NAME=PROTOCOL[,k=v...]` | daemon | Repeatable. `NAME` is any module the daemon will load (well-known or user-configured). `PROTOCOL` is `local`, `tcp`, or `tcp_ssl`. Each occurrence adds one listener to the named module. If the flag is omitted entirely, every well-known module gets a single `local` listener; if it's passed without a `local` entry for `NAME`, a `local` listener is added implicitly so same-host callers always work. |
| `--insecure-tcp` | daemon | Allow `tcp` (plaintext) listeners on a non-loopback host. Without this flag, the daemon refuses to bind such a listener because tokens travel in cleartext. |

The `k=v` pairs after the protocol configure the listener:

| Key | Used by | Description |
|-----|---------|-------------|
| `host` | tcp, tcp_ssl | Bind address. Defaults to `127.0.0.1` for `tcp`. |
| `port` | tcp, tcp_ssl | Port (`0` = auto-assign). |
| `codec` | tcp, tcp_ssl | Wire codec: `json` (default, debuggable) or `cbor` (compact). |
| `cert` | tcp_ssl | Server cert PEM file. |
| `key` | tcp_ssl | Server private key PEM file. |
| `ca` | tcp_ssl | CA cert PEM file. |
| `verify_peer` | tcp_ssl | `true` / `false` — require client cert verification. |

Each well-known module needs its own listener so the host-side client
can dial each. Examples:

```
# TCP — plaintext, good for localhost or trusted networks. Local
# listeners are added implicitly; just name the TCP one for each
# module that needs an outside-facing surface.
--module-transport core_service=tcp,host=127.0.0.1,port=6000
--module-transport capability_module=tcp,host=127.0.0.1,port=6001

# TCP + TLS — wire-encrypted; cert + key required, CA optional. Local
# listeners are still added implicitly.
--module-transport "core_service=tcp_ssl,host=0.0.0.0,port=6443,cert=/p/c.pem,key=/p/k.pem,ca=/p/ca.pem"
--module-transport "capability_module=tcp_ssl,host=0.0.0.0,port=6444,cert=/p/c.pem,key=/p/k.pem,ca=/p/ca.pem"

# Per-module: applies to user modules too. The operator's TCP listener
# is the additional outside-facing surface; same-host callers still
# reach `my_module` over LocalSocket without extra configuration.
--module-transport my_module=tcp,host=127.0.0.1,port=6010
```

#### Client-side dial spec

Client commands never read daemon-only files (`daemon/config.json`,
`daemon/tokens.json`). They read `<configDir>/client/config.json`,
which holds the dial spec (`endpoint`, `host`, `port`, `codec`,
`cert`/`key`/`ca`/`verify_peer` for TLS) and a `token_file` pointing
at the raw-token file alongside. (`status` consults
`daemon/state.json` for a fast same-host liveness check via
`kill(pid, 0)`, but never opens daemon-only secrets.)

The daemon auto-emits `client/config.json` + `client/auto.json` for the
local same-host case on the first boot into an empty config dir — local
clients work out of the box with no manual setup. Subsequent boots leave
an existing `client/config.json` alone (so an operator-written
remote-client config isn't clobbered). For remote clients
(port-forwarded containers, NAT, SSH tunnels) hand-write
`client/config.json` with the right host:port for each module and
reference a `token_file` whose contents was copied from a
`daemon/tokens/<name>.json` on the daemon host.

---

## Commands

### `daemon` / `-D`

Start the daemon process.

```
logoscore -D [--modules-dir <path>]...
logoscore daemon [--modules-dir <path>]...
```

Starts the Logos Core runtime in the foreground. Startup and shutdown messages go to stdout (so `> logs.txt` captures them); debug/info/warning logs go to stderr and are suppressed unless `--verbose` is passed. Writes `~/.logoscore/daemon/state.json` on startup (and on the first fresh boot also emits `~/.logoscore/client/config.json` + `~/.logoscore/client/auto.json` for the local client), removes `state.json` on clean shutdown.

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

Displays daemon state (PID, uptime, version, instance ID, configured listeners) and a summary of all modules with their status. This is the single "dashboard" command — it shows everything at a glance so agents don't need to chain multiple commands.

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

Displays extended metadata: version, status, dependencies, available methods, emitted events, process info (PID, uptime), and crash details if applicable. Methods and events each carry their `description` (from the module's header doc comments) when documented. This is the deep-inspection counterpart to `list-modules`.

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

Sends a shutdown request to the daemon via `core_service`. The daemon performs a clean shutdown: unloads all modules, removes `daemon/state.json`, and exits. The client prints a confirmation message and exits.

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

Issue a new named token and write it to `<configDir>/daemon/tokens/<name>.json`.

```
logoscore issue-token --name <name> [--replace] [--expires <dur>] [--local-only]
```

Appends an entry to `<configDir>/daemon/tokens.json["tokens"]` (a
`{name, hash, issued_at, expires_at, local_only}` row, hashes are SHA-256
hex) and writes a companion raw-value file at `daemon/tokens/<name>.json`
for distribution. Without `--replace`, the command refuses to overwrite an
existing token with the same name so a stale credential isn't silently
invalidated; pass `--replace` to rotate.

`--expires <dur>` sets a TTL after which the daemon rejects the token (e.g.
`30d`, `12h`). `--local-only` marks the token as valid only over LocalSocket,
so even a compromised TCP listener can't replay it.

After copying `daemon/tokens/<name>.json` to the client host (typically into
the client's `<configDir>/client/`), the operator may delete the daemon-side
raw file — the daemon validates against the in-memory map seeded from
`tokens.json["tokens"]`'s hashes, not the raw file. Distribute the raw file
the way you'd distribute a private key; do not commit it to version control.

This command operates directly on the config dir on disk; it doesn't need the
daemon to be running. Operator-issued tokens take effect on the next daemon
restart (SIGHUP-driven reload is a follow-up).

### `revoke-token <name>`

Remove a named token from `<configDir>/daemon/tokens.json["tokens"]`.

```
logoscore revoke-token <name>
```

After this returns, any RPC presenting the revoked token is rejected by the
daemon with an authentication error. The on-disk
`daemon/tokens/<name>.json` file is also removed so clients that still have
it can't mistake it for valid.

### `list-tokens`

List all tokens currently issued against this config dir.

```
logoscore list-tokens
```

Shows token name, issued-at timestamp, expires-at, and the local-only flag —
never the plaintext token, which only lives in the
`daemon/tokens/<name>.json` file at the moment of issuance. Lost a token?
Rotate it with `issue-token --replace`.

---

## Authentication

### How Tokens Work

Logos Core uses UUID-based tokens for authentication. Every module loaded into the runtime receives a unique token generated by the core. These tokens are used to authorize RPC calls between components.

The CLI needs a token to authenticate with the daemon's `core_service`. This token is called the **client token** and is generated by the daemon on startup.

### Token Lifecycle

```
1. DAEMON STARTS
   logoscore -D -m ./modules
   → Daemon mints an "auto" token (local_only=true) for the local client
   → Hash + metadata persisted into ~/.logoscore/daemon/tokens.json["tokens"]
   → Raw value emitted to ~/.logoscore/client/auto.json
   → ~/.logoscore/client/config.json written so local clients dial correctly

2. CLIENT CONNECTS
   logoscore load-module waku
   → Reads ~/.logoscore/client/config.json (dial spec + token_file)
   → Loads the raw token from the file token_file points at
   → Sends token with RPC request to `core_service`
   → `core_service` validates the token's hash against tokens.json["tokens"]
   → Request authorized, module loads

3. REMOTE / PROGRAMMATIC ACCESS
   LOGOSCORE_TOKEN=<token> logoscore load-module waku
   → Token from env var overrides the one in client/config.json's token_file
   → Useful when client/ isn't writable (remote, containers, CI)
```

### Token Resolution Order

When a client command runs, the token is resolved in this order (first match wins):

| Priority | Source | Example |
|----------|--------|---------|
| 1 | `LOGOSCORE_TOKEN` env var | `LOGOSCORE_TOKEN=abc123 logoscore list-modules` |
| 2 | `<configDir>/client/<token_file>` | the path is whatever `client/config.json` says (defaults to `auto.json`) |

A named token issued by `logoscore issue-token --name alice` produces
`<configDir>/daemon/tokens/alice.json` on the daemon host. To use it as a
client on a different machine, copy the file into the client host's
`<configDir>/client/` and reference it via `token_file` in `client/config.json`.
Once copied, the operator may delete the daemon-side raw file — validation
keeps working because the hash is what the daemon checks.

### Obtaining a Token

**Local usage (same machine):** No manual token management needed. At boot
the daemon auto-issues an `auto` token (with `local_only=true`, so it can't
be used over TCP), writes the hash into `daemon/tokens.json["tokens"]`, and
emits the raw value into `client/auto.json` alongside a local-default
`client/config.json`. Local client commands just work.

**Remote or programmatic usage:** Issue a named token on the daemon host and
move it to the client host:

```bash
# On the machine running the daemon:
logoscore issue-token --name alice
cat ~/.logoscore/daemon/tokens/alice.json
# Output: 550e8400-e29b-41d4-a716-446655440000

# On the remote machine or in a script:
export LOGOSCORE_TOKEN=550e8400-e29b-41d4-a716-446655440000
logoscore list-modules --json

# Or persist by copying the file alongside a hand-written client/config.json:
mkdir -p ~/.logoscore/client
scp daemon-host:~/.logoscore/daemon/tokens/alice.json ~/.logoscore/client/alice.json
# then edit ~/.logoscore/client/config.json so token_file = "alice.json"
```

**CI / containers:** Pass the token as an environment variable at runtime:

```bash
docker run -e LOGOSCORE_TOKEN=$TOKEN myimage logoscore list-modules --json
```

### Daemon files (config / state / tokens)

The daemon dir splits by lifetime into three files:

- **`daemon/state.json`** — live runtime state. Written every boot
  (after transports actually bind), removed on clean shutdown.
- **`daemon/config.json`** — operator preferences. Written ONLY when
  the operator passed `--persist-config`; otherwise absent.
- **`daemon/tokens.json`** — hashed-at-rest accepted-token list.
  Independent of the running daemon's lifetime.

#### `daemon/state.json`

```json
{
  "version": 2,
  "instance_id": "a3f1c8d20b4e",
  "pid": 12345,
  "started_at": "2026-03-23T14:00:00Z",
  "config_source": "cli",
  "resolved": {
    "modules_dirs": ["/path/to/modules"],
    "load_modules": "",
    "persistence_path": "/var/lib/logoscore",
    "modules": {
      "core_service": {
        "transports": [
          { "protocol": "local" },
          { "protocol": "tcp",     "host": "0.0.0.0", "port": 6000, "codec": "json" },
          { "protocol": "tcp_ssl", "host": "0.0.0.0", "port": 6443,
            "codec": "cbor", "ca_file": "/etc/logoscore/ca.pem",
            "verify_peer": true }
        ]
      },
      "capability_module": {
        "transports": [
          { "protocol": "local" },
          { "protocol": "tcp", "host": "127.0.0.1", "port": 6001, "codec": "json" }
        ]
      }
    },
    "ssl": { "cert": "", "key": "", "ca": "" },
    "insecure_tcp": false
  }
}
```

- `instance_id` is a 12-char UUID prefix the client uses with `LogosInstance::id()`
  to reconstruct the same registry URL the daemon published (`local:logos_core_service_<id>`).
- `pid` lets co-resident clients detect a stale state file
  (`kill(pid, 0) == ESRCH` after a hard crash).
- `config_source` records where the running daemon's config came from:
  `cli` (any `--module-transport`/`--insecure-tcp`/etc. flag was
  passed), `config.json` (loaded from disk only), or `defaults`.
- `resolved.modules` is the post-bind transport set: `port: 0` in
  config.json becomes the actually-bound port here.
- `resolved` mirrors the shape of `daemon/config.json` (same field set,
  minus `version`).

#### `daemon/config.json` (operator preferences)

Same shape as `state.json`'s `resolved` block, plus `version`. Reflects
*operator intent* — `port: 0` stays `0` (auto-pick) — not the resolved
post-bind values. Written only when `--persist-config` is passed.

#### `daemon/tokens.json`

```json
{
  "version": 2,
  "tokens": [
    { "name": "auto",  "hash": "<sha256-hex>", "issued_at": "...", "expires_at": null, "local_only": true },
    { "name": "alice", "hash": "<sha256-hex>", "issued_at": "...", "expires_at": "...", "local_only": false }
  ]
}
```

One entry per issued token: `{name, hash, issued_at, expires_at,
local_only}`. Hashes are SHA-256 hex; raw values live only in
`daemon/tokens/<name>.json` at issue time. Independent of the running
daemon's lifetime — survives restarts.

These three files are daemon-owned; the client never reads
`config.json` or `tokens.json`, and only consults `state.json` for a
fast same-host liveness check via `kill(pid, 0)`. The client reads
`<configDir>/client/config.json` to learn how to dial. Liveness — *is
the daemon actually answering?* — falls through to the first RPC (e.g.
`status`), so a connect failure surfaces via the same code path as any
other method call.

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
  State file:   /Users/iuri/.logoscore/daemon/state.json

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
    "version": "0.5.0"
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

No daemon state file at /Users/iuri/.logoscore/daemon/state.json
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
      Sends a chat message to the active channel.
  get_history() -> QJsonArray
      Returns the message history for the active channel.
  set_nickname(name: QString) -> bool
  get_status() -> QString

Events:
  message_received(from: QString, body: QString)
      Emitted when a new message arrives on the active channel.
  connection_changed(online: bool)
```

Each method line shows `name(param: type, …) -> returnType`. When a method
carries documentation, its `description` is printed on the following line(s),
indented — a multi-line doc comment keeps its line breaks, one indented line
each. The description originates from the doc comment written directly above the
method's declaration in the module's header (see the module-builder docs);
methods without a doc comment simply omit it.

The **Events** section lists the events the module emits, in the same
`name(param: type, …)` form — but with no return type, since events are
fire-and-forget. An event's `description` (from the doc comment above its
`logos_events:` declaration) is printed indented beneath it, exactly as for
methods. The section is omitted when the module declares no events.

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
    {"name": "send_message", "signature": "send_message(QString)", "returnType": "QString", "isInvokable": true, "description": "Sends a chat message to the active channel.", "parameters": [{"name": "text", "type": "QString"}]},
    {"name": "get_history", "signature": "get_history()", "returnType": "QJsonArray", "isInvokable": true, "description": "Returns the message history for the active channel.", "parameters": []},
    {"name": "set_nickname", "signature": "set_nickname(QString)", "returnType": "bool", "isInvokable": true, "parameters": [{"name": "name", "type": "QString"}]},
    {"name": "get_status", "signature": "get_status()", "returnType": "QString", "isInvokable": true, "parameters": []}
  ],
  "events": [
    {"name": "message_received", "signature": "message_received(QString,QString)", "description": "Emitted when a new message arrives on the active channel.", "parameters": [{"name": "from", "type": "QString"}, {"name": "body", "type": "QString"}]},
    {"name": "connection_changed", "signature": "connection_changed(bool)", "parameters": [{"name": "online", "type": "bool"}]}
  ]
}
```

The `methods` array is the module's `getPluginMethods` introspection, emitted
verbatim. Each entry carries `name`, `signature`, `returnType`, `isInvokable`,
`parameters` (each `{name, type}`), and — when the method is documented —
`description` (sourced from the method's header doc comment).

The `events` array is the module's `getPluginEvents` introspection. Each entry
carries `name`, `signature`, `parameters` (each `{name, type}`), and — when the
event is documented — `description`. There is no `returnType`/`isInvokable`:
events are void. Modules with no declared events report an empty array (legacy
`provider` modules always do).

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

## Daemon + client workflow

Module method calls go through a running daemon. Start one with `-D` (it can
pre-load modules and configure persistence), then use client subcommands:

```bash
# Start a daemon, pre-loading waku + chat from /path
logoscore -D -m /path -l waku,chat &
logoscore load-module waku          # (or load more modules at runtime)
logoscore load-module chat
logoscore call chat send_message "hello"
```

The legacy inline mode (`-c "module.method(args)"` / `--quit-on-finish`, which
started the core, ran calls in one short-lived process, and exited) has been
removed. `-m`/`-l`/`--persistence-path` now apply only to daemon startup (`-D`);
a subcommand operates in client mode and connects to a running daemon.

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
#     {"name": "send_message", "signature": "send_message(QString)", "returnType": "QString", "isInvokable": true, "description": "Sends a chat message to the active channel.", "parameters": [{"name": "text", "type": "QString"}]},
#     {"name": "get_history", "signature": "get_history()", "returnType": "QJsonArray", "isInvokable": true, "description": "Returns the message history for the active channel.", "parameters": []},
#     {"name": "get_status", "signature": "get_status()", "returnType": "QString", "isInvokable": true, "parameters": []}
#   ],
#   "events": [
#     {"name": "message_received", "signature": "message_received(QString,QString)", "description": "Emitted when a new message arrives on the active channel.", "parameters": [{"name": "from", "type": "QString"}, {"name": "body", "type": "QString"}]}
#   ]
# }
# Agent now knows send_message takes a text param and returns a string, and —
# from each method's "description" — what it does, without any external docs.
# The "events" array tells it which events it can watch (and what they mean).

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
| `module-info` with method signatures + descriptions | Agent discovers available operations and their intent — it reads each method's schema and `description` to construct calls without external docs |
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
   → Daemon mints "auto" token (local_only=true) for the local client
   → Scans /path/to/modules for available plugins
   → Writes ~/.logoscore/daemon/state.json (instance + resolved listeners)
   → Writes ~/.logoscore/daemon/tokens.json (hashed accepted-token list)
   → Emits ~/.logoscore/client/config.json + ~/.logoscore/client/auto.json
   → Runs event loop (foreground)

2. LOAD MODULES
   logoscore load-module waku
   → Reads ~/.logoscore/client/config.json (dial spec + token_file)
   → Loads token from the file token_file points at
   → Connects to daemon's `core_service` via RPC with token
   → Daemon resolves dependencies for "waku"
   → Daemon loads dependencies first, then waku
   → Client prints result and exits

3. CALL METHODS
   logoscore call chat send_message "hello"
   → Reads dial spec + token from ~/.logoscore/client/
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
   → Removes ~/.logoscore/daemon/state.json (tokens.json + config.json survive)
   → Exits

   Alternatively: Ctrl+C / kill <pid> / SIGTERM
   → Signal handler triggers QCoreApplication::quit()
   → Same cleanup as above
```

