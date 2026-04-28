#ifndef LOGOSCORE_TOKEN_STORE_H
#define LOGOSCORE_TOKEN_STORE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// TokenStore — per-config-dir token management for logoscore clients.
//
// On disk, two pieces under $CONFIG_DIR/daemon/:
//   - daemon.json["tokens"] — authoritative array of {name, hash, issued_at,
//                              expires_at, local_only}. Hashed at rest so
//                              a read of daemon.json reveals names + issue
//                              timestamps + opaque digests but not the
//                              token itself.
//   - tokens/<name>.json    — operator-copyable file with the *raw* token,
//                              created at issue time. Once the operator
//                              copies it to a client host (or to the local
//                              `client/` dir), they're free to delete the
//                              raw file from the daemon side — the hash in
//                              daemon.json is what validates from then on.
//
// File perms: tokens/ is `0700`, individual files are `0600`.
//
// `issue-token` / `revoke-token` / `list-tokens` subcommands operate on the
// store directly without needing a running daemon. A running daemon re-reads
// daemon.json on startup; in-process changes from issue/revoke take effect
// on the next daemon restart (or future: SIGHUP-driven reload).
// -----------------------------------------------------------------------------

struct IssuedToken {
    std::string name;
    std::string issuedAt;   // ISO 8601 UTC
    std::string expiresAt;  // ISO 8601 UTC; empty = non-expiring
    bool        localOnly = false;
    // Whether the operator-copyable raw file under daemon/tokens/<name>.json
    // is still present. False after the operator has deleted it post-copy;
    // doesn't affect validation either way.
    bool        rawFilePresent = false;
};

class TokenStore {
public:
    explicit TokenStore(std::string configDir);

    // Generate a fresh random token, add it under `name`. Returns the raw
    // token string on success. Persists `{name, hash, issued_at,
    // expires_at, local_only}` to daemon.json's `tokens` array AND writes
    // the raw token to daemon/tokens/<name>.json.
    //
    // `expiresAt` may be empty (non-expiring) or an ISO 8601 absolute
    // deadline. `localOnly` constrains the token to the LocalSocket
    // transport at validation time. If `replace` is false and `name`
    // already has an entry in daemon.json, returns std::nullopt.
    std::optional<std::string> issueToken(const std::string& name,
                                          const std::string& expiresAt = {},
                                          bool localOnly = false,
                                          bool replace = false);

    // Remove the token for `name` from daemon.json's `tokens` array and
    // unlink daemon/tokens/<name>.json (best-effort). Returns true if
    // an entry was removed from daemon.json.
    bool revokeToken(const std::string& name);

    // List every entry in daemon.json's `tokens` array. Hashes are not
    // returned. `rawFilePresent` is filled in by stat()ing each
    // daemon/tokens/<name>.json so callers can flag stale-after-copy
    // entries in human output.
    std::vector<IssuedToken> listTokens() const;

    // Look up a token by its raw value. Hashes the input and matches
    // against daemon.json's `tokens`. On match, additionally enforces:
    //   - expires_at: rejected if `now() >= expires_at`
    //   - local_only: rejected unless `transportProtocol == "local"`
    //
    // `transportProtocol` should be one of the wire-level transport
    // identifiers ("local", "tcp", "tcp_ssl"). Returns the entry name
    // on success.
    std::optional<std::string> lookupByToken(const std::string& token,
                                             const std::string& transportProtocol) const;

    // Absolute path of daemon/tokens/<name>.json — exposed so the CLI
    // can print it to operators after issue.
    std::string rawTokenFilePath(const std::string& name) const;

    // SHA-256 hex digest. Exposed for daemon-internal token validation
    // that wants to compare against the in-memory `DaemonState::tokens`
    // map without going through the file path.
    static std::string hashToken(const std::string& token);

private:
    std::string m_configDir;     // <CONFIG_DIR>
    std::string m_daemonDir;     // <CONFIG_DIR>/daemon
    std::string m_tokensDir;     // <CONFIG_DIR>/daemon/tokens
};

#endif // LOGOSCORE_TOKEN_STORE_H
