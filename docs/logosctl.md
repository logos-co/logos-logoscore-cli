# `logosctl`

The merged CLI: `logoscore` + `lgpd` + `lgpm` in one tool. It runs modules
like [`logoscore`](logoscore.md) does, and it manages packages itself —
bundling the `package_manager` and `package_downloader` modules, so searching
a catalog, installing with dependencies resolved, loading and calling are all
one binary's job.

**This is the binary releases ship.** It shares the runtime with
`logoscore` but no state — its session lives in `~/.logosctl`, so nothing here
can disturb a `logoscore` deployment.

Build instructions, flake outputs and test targets are in the
[main README](../README.md). The `.#ctl-*` outputs are the ones that ship this
binary; the package commands need a **portable** build, because that is what
the public catalog ships.

`logosctl` itself is Qt-free. Local RPC uses `qt_remote_plain`, which speaks
the same wire protocol as current modules built with `qt_remote`; those modules
run unchanged in the separate `logos_host_qt` compatibility process. The
Qt-free runtime also supports `tcp` and `tcp_ssl` listeners and clients.

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
│   ├── startup.err     # transient: a --detach child's output before logging is up
│   ├── state.json      # live instance: pid, instance id, bound ports
│   ├── tokens.json     # hashed-at-rest accepted tokens
│   └── tokens/<name>.json
├── client/
│   ├── config.yaml     # dial spec + token_file — you write this for remote use
│   └── auto.json       # local token, rewritten by the daemon each boot
├── modules/            # core modules installed into this session
├── plugins/            # UI plugins installed into this session
├── keyring/            # trusted package-signing keys
├── logs/               # daemon.log -> daemon_<timestamp>.log, rotated
├── cache/downloads/    # fetched .lgx
└── data/               # per-module persistence
```

A session is **portable by default**: copy the directory and its packages,
catalogs and trust assumptions come with it. Two sessions can hold different
package sets and disagree about which signers they trust. The only things left
outside are the binary itself and the runtime sockets in `$TMPDIR`.

That default is not a cage — any of the subdirectories can be redirected:

```yaml
dirs:
  keyring: ~/.config/logos/trusted-keys   # share trust across sessions
  cache: /var/cache/logos                 # put downloads on a bigger disk
  modules: /opt/logos/modules             # a tree something else manages
  plugins: plugins-custom                 # relative -> stays inside the session
  data: /var/lib/logos/data
  logs: /var/log/logos                    # ship logs where your collector looks
```

How a value is written decides whether portability survives:

| Form | Resolves to | |
|---|---|---|
| `plugins-custom` | `<session>/plugins-custom` | still portable |
| `~/x` | `$HOME/x` | outside the session |
| `/var/cache/logos` | as given | outside the session |

`persistence_path` is the older spelling of `dirs.data` and still works;
`dirs.data` wins if both are set.

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
only after the daemon has published `daemon/state.json`, and tells you where it
is logging.

#### Logs

Everything the daemon and its module subprocesses write goes to a rotating file
under `logs/`:

```yaml
logging:
  enabled: true          # false -> no log file at all
  file: daemon.log       # inside dirs.logs
  max_size_mb: 10        # rotate past this; 0 = never rotate
  max_files: 5           # keep this many in total, oldest dropped
  console: true          # mirror to the terminal (ignored once detached)
```

Each start writes a **new, timestamped file**, the same scheme Basecamp uses:

```
logs/
├── daemon.log -> daemon_20260729_180411.log   # always the current session
├── daemon_20260729_180404.log
├── daemon_20260729_180409.log
└── daemon_20260729_180411.log
```

`logging.file` names the log; the start time is inserted into the real file, and
that exact name survives as a symlink to whichever file is current — so
`tail -F logs/daemon.log` follows across restarts without anyone working out a
stamp.

`max_files` bounds the **directory**, not just one session's rotations: the
oldest files are pruned at each start, so a daemon restarted a hundred times
does not leave a hundred logs. (spdlog's own retention only prunes within a
single sink's rotation set, which is why this is enforced separately.)

Capture is **pipe-based**, not a file redirect, and that is the point: module
hosts are separate processes holding inherited descriptors. Redirecting to a
file would catch their output but make rotation impossible — renaming a file out
from under a child that has it open just keeps filling the old inode. A pipe
puts one reader in charge, so rotation is safe *and* subprocess output still
lands in the log.

The size cap and retention come from [spdlog](https://github.com/gabime/spdlog)'s
rotating sink, which liblogos already logs through. Lines arriving from the pipe
already carry their own timestamp and level, so they are written verbatim rather
than stamped twice.

#### Configuration

Configuration is a YAML document you install into the session. It is never
passed alongside another command, so `daemon start` and every client command act
on the session exactly as it is on disk.

```bash
logosctl daemon config set ./node.yaml     # also accepts @file, or - for stdin
logosctl daemon config show
```

`set` replaces the file wholesale — there is no merge — and validates the whole
document, through the loader the daemon itself uses, before writing anything. A
document it rejects is never installed, so the previous config stays intact and
the session is never left holding one the daemon would refuse to boot from.
Unknown keys are rejected by name, because the loader ignores what it does not
recognise and a silently-dropped `insecureTcp` (the key is `insecure_tcp`) would
leave the daemon running with your intent missing. A key whose *value* is the
wrong type is reported the same way — `modules_dirs: /single/path` (a scalar
where a list belongs) fails with `modules_dirs: expected a list of strings, but
got a string.` rather than being ignored, half-applied, or fatal.

```yaml
# node.yaml
dirs:
  keyring: ~/.config/logos/trusted-keys
  cache: /var/cache/logos
access_group: logos
signature_policy: warn
modules_dirs:
  - /opt/logos/modules
```

The Qt-free runtime accepts local `qt_remote_plain`, `tcp`, and `tcp_ssl`
listeners. The daemon writes the local client dial spec and token into the
session on every boot; remote clients can configure the advertised network
endpoints separately.

#### Reaching a daemon over tcp or tls

The boot token the daemon writes to `client/auto.json` is accepted over the
local socket only, so copying it to another machine no longer works: the
daemon refuses it over `tcp` and `tcp_ssl`, and the client says so with
`UNAUTHORIZED`. Issue a named token for each remote client instead, with an
expiry, on the daemon's host:

```bash
logosctl token issue --name laptop --expires 30d --json   # prints the raw token once
```

On the client, `client/config.yaml` names the network listener (its port is in
the daemon's `daemon/state.json`) and the file holding that token:

```yaml
version: 2
token_file: laptop.json          # client/laptop.json: {"token": "<raw token>"}
daemon:
  core_service:
    transport: tcp_ssl           # or tcp
    host: node.example.org
    port: 6443
    ca: /path/to/ca.pem          # tcp_ssl only
```

`$LOGOSCTL_TOKEN` overrides the token file. `logosctl token revoke laptop`
withdraws it at once; the expiry bounds a token nobody revoked.

> The two documents are kept separate on purpose: the daemon never reads
> `client/`, and the client never reads `daemon/`. Only the files above are
> YAML — everything the daemon and the modules own (`state.json`, `tokens.json`,
> the token files, the downloader's catalog config) stays JSON.

#### Client Commands

Commands are grouped by what they act on. `ls` is the list verb throughout,
`show` the detail verb.

```bash
# Daemon
logosctl daemon start [--detach]     # start the runtime
logosctl daemon stop                 # graceful shutdown
logosctl daemon status               # health, uptime, module summary
logosctl daemon config set FILE      # install/replace the daemon config
logosctl daemon config show          # print it, and where it lives

# Modules — what is running right now
logosctl module ls [--loaded]        # list known / loaded modules
logosctl module show NAME            # methods, events, deps, crash detail
logosctl module load NAME            # + dependencies, always (no opt-out)
logosctl module unload NAME          # + dependents  (--no-dependents to opt out)
logosctl module reload NAME
logosctl module stats                # per-module CPU / memory

# Calling and watching
logosctl call MODULE METHOD [args...]
logosctl watch MODULE [--event NAME]

# Packages — what is on disk
# install/upgrade share one option set:
#   --file X.lgx  --dir D  --version V  --root-hash H  --catalog C
#   -y|--yes  --dry-run  --no-deps  --no-dependents
# remove takes names only:  -y  --dry-run  --no-dependents
logosctl package install NAME|FILE.lgx ...   # a path installs from disk
logosctl package install --dir D             # every .lgx in a directory
logosctl package upgrade NAME|FILE.lgx
logosctl package remove NAME         # + dependents by default
logosctl package ls [--type core|ui]
logosctl package show NAME|FILE.lgx  # detail + every version, or inspect an .lgx
logosctl package deps NAME [-r|--recursive] [--reverse]
logosctl package search [QUERY] [--category C] [--catalog C]
logosctl package download NAME [--version V] [--root-hash H] [--catalog C] [-o|--output DIR]

# Catalogs
logosctl catalog ls
logosctl catalog add URL | remove URL | enable URL | disable URL
logosctl catalog refresh

# Trusted signing keys (per session)
logosctl key ls
logosctl key add NAME --did DID [--display-name N] [--url U]
logosctl key remove NAME

# Auth tokens (offline; operates on the session dir)
logosctl token issue --name N [--expires D] [--replace] [--local-only]
logosctl token ls
logosctl token revoke NAME
```

Shorthands exist for the things you type most: `status`, `stop`, `call`,
`watch`, `stats`, `install`, and `search` work at the top level. `list` is
accepted wherever `ls` is, `info` wherever `show` is, and `uninstall` for
`package remove`.

`install` and `search` are the only package commands aliased to the top level,
because they are the only ones with no runtime-module meaning — `ls`, `show`
and `remove` would each be ambiguous between a package and a loaded module.

`package search` shows one row per package: the newest version, and `(+N)` when
older releases exist. `package show NAME` names every one of them on an
`available:` line, and answers for a package that is only in a catalog as well
as one that is installed. Pass any of those values to `package install` or
`package download` with `--version` to select that exact release; without it,
the newest version is used.

Two defaults are worth stating plainly, because they are the opposite of what
some tools do:

- **`install` does not load.** It puts files on disk; `module load` activates
  them. Staging a package without starting it is a thing you may well want.
- **`remove` takes dependents with it**, and `unload` too. Leaving a module
  running against something that no longer exists is the more surprising
  outcome. `--no-dependents` opts out, acting on the named package or module
  alone and leaving its dependents in place.
- **`module load` always resolves dependencies.** There is no `--no-deps` on
  it — that flag exists only on `package install` / `package upgrade`, which
  are about files on disk, not about what is running.
- **An argument ending in `.lgx` is a path, not a name.** `install` and
  `upgrade` read it as a file on disk and skip the catalog entirely, so a
  local package installs with no catalog configured and no network. `--file`
  says so explicitly and `--dir` takes every `.lgx` in a directory. Catalog
  names and local files cannot be mixed in one command — they resolve by
  different rules, so run them separately.

Installing does not require a daemon restart: the daemon re-scans afterwards,
and only modules that were *already running* are stopped and restarted.

#### Argument typing

Each positional argument to `call` is turned into a JSON value using the first
rule that matches, so scalars stay ergonomic while lists, maps, and literal
strings are all expressible:

| Argument form | Becomes | Example |
|---------------|---------|---------|
| `json:<value>` | the value parsed as JSON (list / map / number / any nested value) | `json:[1,2,3]`, `json:{"k":"v"}` |
| `json:@<file>` | the file's contents parsed as JSON | `json:@payload.json` |
| `str:<text>` | `<text>` verbatim as a string — no parsing, no coercion | `str:json:x` → `"json:x"`, `str:42` → `"42"` |
| `@<file>` | the file's raw contents as a string | `@config.json` |
| `true` / `false` | a boolean | `true` |
| a whole number, in decimal | an integer | `42`, `-7` |
| a decimal number | a double | `3.14`, `1e6` |
| anything else | a string | `hello`, `0xf39F…` |

`json:` and `str:` are the two explicit escapes, mirroring the convention used
by `jq` (`--arg` / `--argjson`) and HTTPie (`=` / `:=`): the default path never
guesses a container, `json:` opts into parsing, and `str:` forces a literal
string for any value the default rules would otherwise reinterpret (a
number-like string, or one that itself starts with `json:` / `str:` / `@`).

Only decimal notation counts as a number. Hex stays a string: an address or
transaction hash such as `0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266` reaches
the module verbatim, with no `str:` needed. So do `inf` and `nan`, which JSON
cannot carry. `str:` remains the escape for a value that *is* decimal but must
arrive as text: `str:42`, `str:1e6`.

**Binary (`bstr`) arguments.** JSON has no native byte type, so bytes use the
canonical tagged encoding — a JSON object `{"_bytes": "<base64url, unpadded>"}`.
Pass it like any other JSON value with `json:`:

```bash
# base64url("hello") == "aGVsbG8"
logosctl call blobstore put 'json:{"_bytes":"aGVsbG8"}'
logosctl call blobstore put 'json:@blob.json'   # {"_bytes":"..."} from a file
```

#### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | General error / daemon not running (for `status`) |
| `2` | No daemon running |
| `3` | Module error (not found, load/unload failed) |
| `4` | Method error (not found, call failed, timeout) |

A session left behind by a daemon that is no longer there is exit 2 as well,
and is reported immediately rather than after the RPC deadline — whether the
daemon crashed:

```
$ logosctl module ls --json
{"status":"error","code":"NO_DAEMON","message":"No daemon running (stale state file: pid 51203 is gone)."}
```

or was stopped normally, which leaves no pid to name and is settled by the
socket instead:

```
$ logosctl module ls --json
{"status":"error","code":"NO_DAEMON","message":"No daemon running (no local endpoint at /tmp/logos_core_service_a1b2c3d4e5f6). A daemon that stopped removes it; start one in this session to get it back."}
```

Start a daemon (`logosctl daemon start`) and both are restored; nothing needs
cleaning up by hand. A client dialing a *remote* daemon is unaffected by either
check.

`module ls` and `stats` report an unanswered RPC as `DAEMON_UNREACHABLE`
(exit 2), never as an empty list — `[]` means the daemon answered and nothing
matched.


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

A package you already have on disk needs none of that — no catalog, no
network. Give `install` the path:

```bash
logosctl install ./storage_module.lgx -y      # or: --file ./storage_module.lgx
logosctl install --dir ./downloads -y         # every .lgx in a directory
logosctl package show ./storage_module.lgx    # inspect one without installing
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
# Start daemon (--detach returns only once it is accepting commands,
# so no `&` and no sleep are needed)
logosctl daemon start --detach

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
# Start the daemon (the modules it scans come from the session's
# modules/ directory and any modules_dirs in daemon/config.yaml)
logosctl daemon start --detach

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
on its own). Load modules with `module load` — transitive dependencies are
resolved automatically — then call methods:

```bash
# Start a clean daemon on this session's modules
logosctl daemon start --detach

# Load modules (deps resolved automatically)
logosctl module load waku
logosctl module load chat

# Call methods (positional args — see "Argument typing" above)
logosctl call chat send_message hello
logosctl call storage init config 42 true
logosctl call storage loadConfig @config.json           # @file → raw file contents
logosctl call storage setTags 'json:["a","b"]'          # json: → parsed list/map/value
logosctl call storage setLabel 'str:42'                 # str: → literal string "42"

# A throwaway session, isolated from ~/.logosctl
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

These global flags work before or after any subcommand:

```
  -h, --help            Show help
      --version         Print the version
  -v, --verbose         Show debug logs
  -j, --json            Force JSON output
      --no-json,--human Force human-readable output even when piped
  -q, --quiet           Suppress non-essential output
      --config-dir DIR  Which session to act on (also LOGOSCTL_CONFIG_DIR)
```

#### Access policy

The `access_policy` key declares, per target module, which caller modules may
invoke it. Unlike every other key in this document, its value is **a JSON
document carried as a string** — the daemon hands the text straight to the
runtime rather than interpreting it, so it is not modelled as YAML. A YAML
block scalar (`|`) is the readable way to write it:

```yaml
access_policy: |
  {
    "version": 1,
    "mode": "enforce",
    "restrictions": {
      "package_manager":    { "allowedCallers": ["package_manager_ui"] },
      "package_downloader": { "allowedCallers": ["package_manager_ui"] }
    }
  }
```

> **Do not write it as a YAML mapping.** `access_policy:` followed by an
> indented `version: 1` / `restrictions:` block is read as an object where a
> string is expected. `daemon config set` reports it by name
> (`access_policy: expected a string, but got a mapping.`) and writes nothing.
> Quote the JSON, or use the `|` block above.

The string is handed to the runtime (via `logos_core_set_access_policy`)
before any module is loaded.

> **Note:** the value must be a full policy document. The `enforce` shorthand
> is a `logoscore` flag spelling (`--access-policy enforce`); here, write the
> equivalent document — `{"version":1,"mode":"enforce","restrictions":{}}` —
> which arms deny-by-default with the allow-lists derived from each module's
> declared dependencies.

> **Note:** the legacy inline mode (`-c "module.method(args)"` / `--quit-on-finish`,
> which ran calls in a single short-lived process) has been removed. Use a daemon
> plus `logosctl call ...` as shown above.
