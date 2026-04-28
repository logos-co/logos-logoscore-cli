#ifndef DAEMON_STATE_H
#define DAEMON_STATE_H

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

// daemon/daemon.json schema version. Bumped from v1 (single flat
// `transports` list + a `token` field) to v2 (per-module map + hashed
// `tokens` array). v1 readers see v2 and refuse to parse, instructing
// the operator to relaunch the daemon to regenerate.
constexpr int kDaemonStateSchemaVersion = 2;

// Transport endpoint advertised by the daemon for one module. Clients
// read the per-module `transports` array and pick one (default: local;
// remote clients pick a tcp / tcp_ssl entry by hand or via flags).
struct TransportInfo {
    std::string protocol;   // "local" | "tcp" | "tcp_ssl"
    std::string host;       // bind address (e.g. "0.0.0.0" or "127.0.0.1")
    uint16_t    port = 0;
    // tcp_ssl: CA cert for verification. May appear in daemon.json
    // because clients need it to verify the server's chain.
    std::string caFile;
    bool        verifyPeer = true;
    // Wire codec for RPC framing (e.g. "json", "cbor"). Only meaningful
    // for plain transports; clients must match what the daemon
    // advertised. Local uses QRO's own format and ignores this.
    std::string codec = "json";
    // tcp_ssl: server reads cert + key from local disk. These are
    // populated only on the daemon side from --ssl-cert / --ssl-key
    // and are deliberately NOT serialized to daemon.json — clients
    // don't need them, and key paths in particular are local secrets.
    std::string certFile;
    std::string keyFile;
};

// One accepted-token entry. Hashes-at-rest (the raw token only ever
// lives in `daemon/tokens/<name>.json` and the operator's copy on the
// client side). Validation: hash the inbound token, compare to `hash`,
// then enforce `expiresAt` (if set) and `localOnly` (if true, the
// inbound transport must be "local").
struct TokenEntry {
    std::string name;
    std::string hash;       // sha256 hex digest of the raw token
    std::string issuedAt;   // ISO 8601 UTC
    // Optional ISO 8601 absolute deadline. Empty = non-expiring.
    std::string expiresAt;
    // If true, the daemon rejects this token over non-local transports.
    bool        localOnly = false;
};

// In-memory representation of <configDir>/daemon/daemon.json. Owned
// and rewritten by the daemon. The client side never touches this
// file (or this struct); see ClientState for the client-owned
// counterpart.
struct DaemonState {
    bool fileOk = false;
    int  schemaVersion = 0;

    // Ephemeral fields (refreshed every daemon boot).
    std::string instanceId;
    int64_t     pid = -1;
    std::string startedAt;

    // Durable config — round-trips through CLI args. When the daemon is
    // launched without flags, these come from disk.
    std::vector<std::string> modulesDirs;
    std::string              loadModules;
    std::string              persistencePath;
    // Per-module advertised transport set. `core_service` and
    // `capability_module` always present after a successful boot.
    std::map<std::string, std::vector<TransportInfo>> modules;
    // Server-side TLS material (paths). Stripped from the wire — the
    // daemon writes these so the next daemon boot without flags can
    // re-bind. Clients don't need them and never consult this file.
    std::string sslCert;
    std::string sslKey;
    std::string sslCa;

    // Authoritative accepted-token list (hashed). Validation goes
    // through this map; the raw files under daemon/tokens/<name>.json
    // are operator-copyable artifacts only.
    std::vector<TokenEntry> tokens;
};

// Format the current UTC time as ISO 8601 (e.g. "2026-04-28T12:34:56Z").
// Exposed so daemon.cpp can stamp `started_at` without duplicating the
// chrono boilerplate.
std::string currentUtcIso8601();

class DaemonStateFile {
public:
    static std::string filePath();

    // Pure parse — no liveness check. `fileOk` reflects whether the file
    // is on disk and shape-valid (correct schema version, non-empty
    // instance_id).
    static DaemonState read();

    // Write the entire state. Caller is responsible for refreshing
    // `pid` / `startedAt` before calling. Returns false on any I/O
    // or serialization error.
    static bool write(const DaemonState& state);

    static bool remove();

    // Emit <configDir>/client/client.json + <configDir>/client/auto.json
    // populated for a local same-host client over LocalSocket. Called by
    // the daemon at boot after auto-issuing the `auto` token; allows
    // `logoscore status` (and friends) to work out of the box from the
    // same machine without hand-writing a client config.
    //
    // For remote operators: don't try to stretch this — copy a
    // daemon/tokens/<name>.json file and hand-write a client.json
    // pointing at the daemon's TCP/TLS endpoint. By design.
    //
    // `autoTokenRaw` is the raw token returned from
    // TokenStore::issueToken — written verbatim into client/auto.json
    // so a `cp` operation could replicate it cleanly.
    static bool writeLocalClientArtifacts(const std::string& instanceId,
                                          const std::string& autoTokenRaw,
                                          const std::string& issuedAt);
};

#endif // DAEMON_STATE_H
