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
//   - tokens.json         — authoritative array of {name, hash, issued_at,
//                            expires_at, local_only}. Hashed at rest so
//                            a read of tokens.json reveals names + issue
//                            timestamps + opaque digests but not the
//                            token itself. Owned by TokenStore; its
//                            lifetime is independent of state.json
//                            (ephemeral) and config.json (preferences).
//   - tokens/<name>.json  — operator-copyable file with the *raw* token,
//                            created at issue time. Once the operator
//                            copies it to a client host (or to the local
//                            `client/` dir), they're free to delete the
//                            raw file from the daemon side — the hash in
//                            tokens.json is what validates from then on.
//
// File perms: tokens.json is `0600`; tokens/ dir is `0700`; individual
// raw-token files are `0600`.
//
// `issue-token` / `revoke-token` / `list-tokens` subcommands operate on the
// store directly without needing a running daemon. A running daemon re-reads
// tokens.json on startup; in-process changes from issue/revoke take effect
// on the next daemon restart (or future: SIGHUP-driven reload).
// -----------------------------------------------------------------------------

// Schema version for daemon/tokens.json. The file is a small standalone
// document (no migration code needed today): readers reject any other
// version with a "regenerate by reissuing tokens" message.
constexpr int kTokensFileSchemaVersion = 2;

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

// Static IO helper for daemon/tokens.json — parallels DaemonStateFile
// but for the persistent-tokens file. Atomic writes (write-temp +
// rename), 0600 perms.
class TokensFile {
public:
    static std::string filePath();
    // Returns an empty vector if the file is missing or unparseable.
    // Callers can disambiguate by stat()ing the path themselves; for the
    // hot path (issue/revoke/list/lookup) "missing" and "empty" are the
    // same outcome.
    static std::vector<TokenEntry> read();
    // Atomic full-file write. Returns false on any I/O error.
    static bool write(const std::vector<TokenEntry>& tokens);
};

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
    // Constructor takes no arguments — paths come from the
    // process-global Config (Config::daemonTokensDir() for the raw
    // per-token files, Config::daemonTokensPath() for the hashed
    // index). A previous design accepted an explicit configDir, but
    // TokensFile already routes through Config::*, so divergence
    // between the two would silently split raw files from hashes
    // into different trees. Tests can still isolate state by setting
    // LOGOSCORE_CONFIG_DIR or calling Config::setConfigDir() in
    // their fixtures.
    TokenStore();

    // Outcome of `issueToken`. CLI dispatch keys off `status`; the
    // `token` field is populated only on `Ok`. Distinguishing
    // `AlreadyExists` from `IoError` matters because operators see
    // very different fixes — re-run with `--replace` vs check
    // disk/permissions.
    enum class IssueStatus {
        Ok,
        InvalidName,    // name failed isSafeName (slashes, traversal, ...)
        AlreadyExists,  // name has a row in tokens.json and replace==false
        IoError,        // raw-file or tokens.json write failed
    };
    struct IssueResult {
        IssueStatus status;
        std::string token;   // only valid when status == Ok
    };

    // Generate a fresh random token, add it under `name`. Persists
    // `{name, hash, issued_at, expires_at, local_only}` to
    // daemon/tokens.json AND writes the raw token to
    // daemon/tokens/<name>.json on success.
    //
    // `expiresAt` may be empty (non-expiring) or an ISO 8601 absolute
    // deadline. `localOnly` constrains the token to the LocalSocket
    // transport at validation time. If `replace` is false and `name`
    // already exists, returns `AlreadyExists`. I/O failures (perm
    // denied, disk full, etc.) return `IoError` so the CLI can
    // surface a different exit code than "name collision".
    IssueResult issueToken(const std::string& name,
                           const std::string& expiresAt = {},
                           bool localOnly = false,
                           bool replace = false);

    // Outcome of `revokeToken`. Same separation rationale as
    // `IssueResult`: CLI shouldn't mask a permission/disk failure
    // as "no such token".
    enum class RevokeStatus {
        Ok,
        InvalidName,
        NotFound,
        IoError,
    };

    // Remove the token for `name` from daemon/tokens.json and
    // unlink daemon/tokens/<name>.json (best-effort). Raw-file
    // unlink failures are NOT IoError — by design the operator may
    // have already deleted that file post-copy.
    RevokeStatus revokeToken(const std::string& name);

    // List every entry in daemon/tokens.json. Hashes are not
    // returned. `rawFilePresent` is filled in by stat()ing each
    // daemon/tokens/<name>.json so callers can flag stale-after-copy
    // entries in human output.
    std::vector<IssuedToken> listTokens() const;

    // Look up a token by its raw value. Hashes the input and matches
    // against daemon/tokens.json. On match, additionally enforces:
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
    // that wants to compare against the in-memory token list without
    // going through the file path.
    static std::string hashToken(const std::string& token);

};

#endif // LOGOSCORE_TOKEN_STORE_H
