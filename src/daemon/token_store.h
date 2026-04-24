#ifndef LOGOSCORE_TOKEN_STORE_H
#define LOGOSCORE_TOKEN_STORE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// TokenStore — per-config-dir store of { name → token } for logoscore clients.
//
// Two pieces on disk under $CONFIG_DIR:
//   - tokens.db        : authoritative JSON map, { hash(token): {name, issued_at} }
//   - tokens/<name>.json : convenience file given to each client so they can
//                          `--token-file $PATH` instead of memorising the token
//
// Tokens are stored hashed at rest (SHA-256) so a read of tokens.db leaks
// names + issue timestamps, never the token itself; revoking requires only
// the name. The per-client files are the ONLY place the raw token lives.
//
// This store is standalone: the `issue-token` / `revoke-token` / `list-tokens`
// subcommands operate on it directly, without needing a running daemon. A
// running daemon re-reads the store at startup and on SIGHUP (follow-up).
// -----------------------------------------------------------------------------

struct IssuedToken {
    std::string name;
    std::string issuedAt;   // ISO 8601 UTC
};

class TokenStore {
public:
    explicit TokenStore(std::string configDir);

    // Generate a fresh random token, add it under `name`, return the raw
    // token string (caller persists it into tokens/<name>.json via
    // writeClientFile() below). If `name` already exists and `replace` is
    // false, returns std::nullopt.
    std::optional<std::string> issueToken(const std::string& name,
                                          bool replace = false);

    // Remove the token for `name`. Returns true if a token was removed.
    bool revokeToken(const std::string& name);

    // List every issued (name, timestamp) — tokens themselves are not
    // returned (they're hashed at rest).
    std::vector<IssuedToken> listTokens() const;

    // Look up a token by its raw value; returns the name if valid.
    std::optional<std::string> lookupByToken(const std::string& token) const;

    // Write / read the per-client convenience file. Endpoint path is
    // relative to the config dir (usually just "daemon.json").
    bool writeClientFile(const std::string& name,
                         const std::string& token,
                         const std::string& endpointPath = "daemon.json") const;

    // Absolute path of the tokens/<name>.json file (for printing).
    std::string clientFilePath(const std::string& name) const;

    // Hash helper — exposed so CoreServiceImpl can validate tokens without
    // loading the full store.
    static std::string hashToken(const std::string& token);

private:
    std::string m_configDir;
    std::string m_dbPath;
    std::string m_tokensDir;

    // Load the db map. { token_hash: {name, issued_at} }.
    struct Entry { std::string name; std::string issuedAt; };
    std::vector<std::pair<std::string, Entry>> loadDb() const;
    bool saveDb(const std::vector<std::pair<std::string, Entry>>& db) const;
};

#endif // LOGOSCORE_TOKEN_STORE_H
