#ifndef DAEMON_STATE_H
#define DAEMON_STATE_H

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

// `daemon/config.json` (operator preferences) and `daemon/state.json`
// (live runtime state). Both use the same v2 schema number — the
// previous unified daemon/daemon.json v2 was internal-only, so the version
// is reused for the split layout.
constexpr int kDaemonConfigSchemaVersion       = 2;
constexpr int kDaemonRuntimeStateSchemaVersion = 2;

// Transport endpoint advertised by the daemon for one module. Clients
// read the per-module `transports` array and pick one (default: local;
// remote clients pick a tcp / tcp_ssl entry by hand or via flags).
struct TransportInfo {
    std::string protocol;   // "local" | "tcp" | "tcp_ssl"
    std::string host;       // bind address (e.g. "0.0.0.0" or "127.0.0.1")
    uint16_t    port = 0;
    // tcp_ssl: CA cert for verification. May appear in state.json
    // because clients need it to verify the server's chain.
    std::string caFile;
    bool        verifyPeer = true;
    // Wire codec for RPC framing (e.g. "json", "cbor"). Only meaningful
    // for plain transports; clients must match what the daemon
    // advertised. Local uses QRO's own format and ignores this.
    std::string codec = "json";
    // tcp_ssl: server reads cert + key from local disk. These are
    // populated only on the daemon side from --ssl-cert / --ssl-key
    // and are deliberately NOT serialized to state.json — clients
    // don't need them, and key paths in particular are local secrets.
    std::string certFile;
    std::string keyFile;
};

// Operator-typed preferences. Persists to daemon/config.json on
// `--persist-config`. Defaults supplied by the merge layer in main.
// Values reflect intent (e.g. `port: 0` stays `0` — the resolved
// port lives in DaemonRuntimeState.resolved).
struct DaemonConfig {
    std::vector<std::string> modulesDirs;
    std::string              loadModules;
    std::string              persistencePath;
    // Per-module advertised transport set. Built from
    // `--module-transport` CLI flags (and the previous-launch config
    // when present). `core_service` and `capability_module` always
    // get an entry by the time the merge resolves; the merge layer
    // fills in the LocalSocket-only default when neither operator
    // input nor config.json supplied one.
    std::map<std::string, std::vector<TransportInfo>> modules;
    // Server-side TLS material (paths). Stripped from state.json's
    // advertised transport set — the daemon needs them to bind, but
    // clients don't need (and shouldn't see) the key path.
    std::string sslCert;
    std::string sslKey;
    std::string sslCa;
    // Mirrors --insecure-tcp. Persisted so operators don't have to
    // retype it next launch. False by default.
    bool insecureTcp = false;
};

// Live-instance runtime state. Written to daemon/state.json on every
// successful boot (after transports actually bind), removed at clean
// shutdown. Stale state.json after a crash is tolerable: the next
// boot overwrites it; co-resident clients can detect "no live daemon"
// by `kill(state.pid, 0) == ESRCH`.
struct DaemonRuntimeState {
    bool fileOk = false;
    int  schemaVersion = 0;

    // Ephemeral identity for this process.
    std::string instanceId;
    int64_t     pid = -1;
    std::string startedAt;
    // Diagnostic: which layer supplied the highest-precedence value
    // for this run. One of "cli", "config.json", "defaults".
    std::string configSource;

    // Resolved post-merge, post-bind values. Same shape as
    // DaemonConfig but with port=0 replaced by the actually-bound
    // port and any "missing well-known module" gaps filled in.
    DaemonConfig resolved;
};

// Format the current UTC time as ISO 8601 (e.g. "2026-04-28T12:34:56Z").
// Exposed so daemon.cpp can stamp `started_at` without duplicating the
// chrono boilerplate.
std::string currentUtcIso8601();

// daemon/config.json — operator preferences. read() returns nullopt
// when the file is missing or its schema version doesn't match;
// callers fall through to defaults. write() is atomic (write-temp +
// rename) and creates the parent dir.
class DaemonConfigFile {
public:
    static std::string filePath();
    static std::optional<DaemonConfig> read();
    static bool write(const DaemonConfig& cfg);
};

// daemon/state.json — live runtime state. Written on every boot,
// removed at shutdown. read()'s `fileOk` flag is true iff a daemon
// has announced itself (instance_id populated). remove() is the
// shutdown hook.
class DaemonRuntimeStateFile {
public:
    static std::string filePath();
    static DaemonRuntimeState read();
    static bool write(const DaemonRuntimeState& state);
    static bool remove();

    // Emit <configDir>/client/config.json + <configDir>/client/auto.json
    // populated for a local same-host client over LocalSocket. Called by
    // the daemon at boot after auto-issuing the `auto` token; allows
    // `logoscore status` (and friends) to work out of the box from the
    // same machine without hand-writing a client config.
    //
    // Two gates control what gets written:
    //
    //   - The raw client/auto.json is **always** (re)written. The
    //     daemon just (re)issued the auto token; any pre-existing
    //     client/auto.json now holds a stale value, and operators who
    //     reference auto.json from a hand-written client/config.json
    //     deserve to keep working.
    //
    //   - The default client/config.json is written **only** when both
    //     conditions hold: client/config.json doesn't already exist,
    //     AND `freshTokensFile` is true (daemon/tokens.json was
    //     created during this very boot). The first gate keeps a
    //     hand-written remote-client config from being clobbered.
    //     The second gate keeps the daemon out of the operator's way
    //     once they've started managing tokens — a returning operator
    //     who deleted client/config.json gets to write their own
    //     instead of having a default LocalSocket config silently
    //     reappear.
    //
    // For remote operators: don't try to stretch this — copy a
    // daemon/tokens/<name>.json file and hand-write a client config
    // pointing at the daemon's TCP/TLS endpoint. By design.
    //
    // `autoTokenRaw` is the raw token returned from
    // TokenStore::issueToken — written verbatim into client/auto.json
    // so a `cp` operation could replicate it cleanly.
    static bool writeLocalClientArtifacts(const std::string& instanceId,
                                          const std::string& autoTokenRaw,
                                          const std::string& issuedAt,
                                          bool               freshTokensFile);
};

#endif // DAEMON_STATE_H
