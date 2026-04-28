#include "token_store.h"
#include "daemon_state.h"

#include <nlohmann/json.hpp>
#include <openssl/sha.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// 32 bytes of URL-safe base64-ish entropy, fine for client tokens.
std::string generateToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    s.reserve(43);
    for (int i = 0; i < 43; ++i) s.push_back(alpha[dist(gen) % 64]);
    return s;
}

// Token names map directly to filesystem paths under
// $CONFIG_DIR/daemon/tokens/. Allow only a strict subset that can't
// traverse out — without this, "../etc/passwd" would let issueToken
// or revokeToken touch arbitrary files on disk.
bool isSafeName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name.front() == '.' || name.front() == '-') return false;
    for (char c : name) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c))
                     || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    if (name == "." || name == "..") return false;
    if (name.find("..") != std::string::npos) return false;
    return true;
}

bool isExpired(const std::string& expiresAt) {
    if (expiresAt.empty()) return false;
    // Parse ISO 8601 UTC (e.g. "2026-12-31T23:59:59Z"). Anything that
    // doesn't parse as exactly that shape is treated as non-expiring
    // rather than rejected — the CLI is responsible for validation
    // when the operator types the date; this path is the safety net.
    struct tm tm{};
    if (!strptime(expiresAt.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) return false;
    // timegm interprets `tm` as UTC.
    time_t deadline = timegm(&tm);
    return std::chrono::system_clock::to_time_t(
               std::chrono::system_clock::now()) >= deadline;
}

} // anonymous namespace

// -- hashToken --------------------------------------------------------------
//
// SHA-256 hex digest. Used as the lookup key in daemon.json["tokens"] so an
// attacker who reads daemon.json can't recover plaintext tokens, and so
// that two distinct tokens can never map to the same entry. File-system
// permissions (mode 0700 dir, 0600 files) remain the primary defense
// against disk exfiltration.
std::string TokenStore::hashToken(const std::string& token)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(token.data()),
           token.size(), digest);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char b : digest) ss << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

// -- Construction -----------------------------------------------------------

TokenStore::TokenStore(std::string configDir)
    : m_configDir(std::move(configDir))
    , m_daemonDir(m_configDir + "/daemon")
    , m_tokensDir(m_daemonDir + "/tokens")
{
    std::error_code ec;
    fs::create_directories(m_tokensDir, ec);
    // Belt-and-braces dir perms (0700) so umask can't widen.
    if (!ec) ::chmod(m_tokensDir.c_str(), S_IRWXU);
}

namespace {

// Helper: load the current DaemonState. Used by issue/revoke for
// read-modify-write of the `tokens` array.
//
// Note: we deliberately don't gate on `fileOk` here. `fileOk` in
// DaemonStateFile::read means "a running daemon is announcing itself"
// (instance_id present); but `issue-token` runs as a standalone CLI
// before the daemon ever starts and writes a state with no
// instance_id. We still want subsequent issue/revoke calls to see the
// tokens that earlier ones wrote, so we trust whatever parsed cleanly
// (schemaVersion == kDaemonStateSchemaVersion) regardless of fileOk.
DaemonState loadOrInit()
{
    DaemonState s = DaemonStateFile::read();
    if (s.schemaVersion != kDaemonStateSchemaVersion) {
        // No file on disk, or unparseable. Start with a blank state
        // so issueToken can persist; the subsequent write fills in
        // daemon.json so future invocations round-trip the `tokens`
        // array.
        s = DaemonState{};
    }
    return s;
}

bool persistTokens(DaemonState& s)
{
    // Refresh ephemeral fields if not already set. issue-token without
    // a running daemon shouldn't claim its pid or boot timestamp.
    if (s.startedAt.empty()) s.startedAt = currentUtcIso8601();
    return DaemonStateFile::write(s);
}

bool writeRawTokenFile(const std::string& path,
                       const std::string& name,
                       const std::string& token,
                       const std::string& issuedAt)
{
    json j;
    j["version"] = 1;
    j["name"] = name;
    j["token"] = token;
    j["issued_at"] = issuedAt;

    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) return false;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return false;
    ofs << j.dump(4) << "\n";
    if (!ofs.good()) return false;
    ofs.close();
    // Tokens are credentials; force restrictive perms regardless of
    // umask so we never accidentally create world/group-readable
    // credential files.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

} // namespace

// -- issue / revoke / list --------------------------------------------------

std::optional<std::string>
TokenStore::issueToken(const std::string& name,
                       const std::string& expiresAt,
                       bool localOnly,
                       bool replace)
{
    if (!isSafeName(name)) return std::nullopt;

    DaemonState s = loadOrInit();
    auto it = std::find_if(s.tokens.begin(), s.tokens.end(),
        [&](const TokenEntry& e) { return e.name == name; });
    if (it != s.tokens.end()) {
        if (!replace) return std::nullopt;
        s.tokens.erase(it);
    }

    const std::string raw = generateToken();
    const std::string issuedAt = currentUtcIso8601();

    TokenEntry e;
    e.name      = name;
    e.hash      = hashToken(raw);
    e.issuedAt  = issuedAt;
    e.expiresAt = expiresAt;
    e.localOnly = localOnly;
    s.tokens.push_back(e);

    if (!persistTokens(s)) return std::nullopt;
    if (!writeRawTokenFile(rawTokenFilePath(name), name, raw, issuedAt))
        return std::nullopt;

    return raw;
}

bool TokenStore::revokeToken(const std::string& name)
{
    if (!isSafeName(name)) return false;

    DaemonState s = loadOrInit();
    const auto before = s.tokens.size();
    s.tokens.erase(
        std::remove_if(s.tokens.begin(), s.tokens.end(),
                       [&](const TokenEntry& e) { return e.name == name; }),
        s.tokens.end());
    if (s.tokens.size() == before) return false;
    if (!persistTokens(s)) return false;

    // Best-effort remove the operator-copyable raw file. It may already
    // be gone (the operator deleted it post-copy, by design).
    std::error_code ec;
    fs::remove(fs::path(rawTokenFilePath(name)), ec);
    return true;
}

std::vector<IssuedToken> TokenStore::listTokens() const
{
    std::vector<IssuedToken> out;
    DaemonState s = DaemonStateFile::read();
    out.reserve(s.tokens.size());
    for (const auto& e : s.tokens) {
        std::error_code ec;
        const bool present = fs::exists(fs::path(rawTokenFilePath(e.name)), ec);
        out.push_back({e.name, e.issuedAt, e.expiresAt, e.localOnly, present});
    }
    return out;
}

std::optional<std::string>
TokenStore::lookupByToken(const std::string& token,
                          const std::string& transportProtocol) const
{
    const std::string h = hashToken(token);
    DaemonState s = DaemonStateFile::read();
    for (const auto& e : s.tokens) {
        if (e.hash != h) continue;
        if (isExpired(e.expiresAt))                 return std::nullopt;
        if (e.localOnly && transportProtocol != "local")
            return std::nullopt;
        return e.name;
    }
    return std::nullopt;
}

std::string TokenStore::rawTokenFilePath(const std::string& name) const
{
    return m_tokensDir + "/" + name + ".json";
}
