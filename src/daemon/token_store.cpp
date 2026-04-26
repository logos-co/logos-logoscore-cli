#include "token_store.h"

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

std::string utcIso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

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

// Token names map directly to filesystem paths under $CONFIG_DIR/tokens/.
// Allow only a strict subset that can't traverse out (no `/`, no `..`,
// no leading `.`) — without this, a name like "../etc/passwd" would let
// issueToken/revokeToken touch arbitrary files on disk.
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

} // anonymous namespace

// ── hashToken ──────────────────────────────────────────────────────────────
//
// SHA-256 hex digest. Used as the lookup key in tokens.db so that an
// attacker who reads the on-disk db can't recover plaintext tokens, and
// — more importantly — so that two distinct tokens can never map to the
// same db entry. The previous 64-bit FNV-1a was non-cryptographic and
// allowed birthday-style collisions which, in an authentication path,
// would let one token validate as a different name. Cryptographic
// collision resistance closes that hole; the file-system permissions
// (mode 0700 dir, 0600 files) remain the primary defense against
// disk exfiltration.
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

// ── Construction ───────────────────────────────────────────────────────────

TokenStore::TokenStore(std::string configDir)
    : m_configDir(std::move(configDir))
    , m_dbPath(m_configDir + "/tokens.db")
    , m_tokensDir(m_configDir + "/tokens")
{
    std::error_code ec;
    fs::create_directories(m_tokensDir, ec);
}

// ── issue / revoke / list ──────────────────────────────────────────────────

std::optional<std::string>
TokenStore::issueToken(const std::string& name, bool replace)
{
    if (!isSafeName(name)) return std::nullopt;

    auto db = loadDb();
    auto existing = std::find_if(db.begin(), db.end(),
        [&](const auto& kv) { return kv.second.name == name; });
    if (existing != db.end()) {
        if (!replace) return std::nullopt;
        // Replace: drop the existing entry now (separate from iteration —
        // the previous range-for-then-erase pattern modified the vector
        // mid-iteration, which is undefined behaviour even with the
        // immediate break that followed).
        db.erase(std::remove_if(db.begin(), db.end(),
                                [&](const auto& kv) { return kv.second.name == name; }),
                 db.end());
    }

    std::string token = generateToken();
    Entry e{name, utcIso8601()};
    db.emplace_back(hashToken(token), std::move(e));

    if (!saveDb(db)) return std::nullopt;
    return token;
}

bool TokenStore::revokeToken(const std::string& name)
{
    if (!isSafeName(name)) return false;

    auto db = loadDb();
    const auto before = db.size();
    db.erase(std::remove_if(db.begin(), db.end(),
                            [&](const auto& kv) { return kv.second.name == name; }),
             db.end());
    if (db.size() == before) return false;
    // Don't claim revocation succeeded if the on-disk db couldn't be
    // rewritten — otherwise the caller reports success while the token
    // is still valid in tokens.db on disk, and we'd then delete the
    // client file leaving the user no way to recover.
    if (!saveDb(db)) return false;

    // Best-effort remove the client file (not fatal on failure).
    std::error_code ec;
    fs::remove(fs::path(clientFilePath(name)), ec);
    return true;
}

std::vector<IssuedToken> TokenStore::listTokens() const
{
    std::vector<IssuedToken> out;
    for (auto& [_hash, e] : loadDb()) {
        out.push_back({e.name, e.issuedAt});
    }
    return out;
}

std::optional<std::string>
TokenStore::lookupByToken(const std::string& token) const
{
    const std::string h = hashToken(token);
    for (const auto& [hash, e] : loadDb()) {
        if (hash == h) return e.name;
    }
    return std::nullopt;
}

// ── Client file (tokens/<name>.json) ───────────────────────────────────────

std::string TokenStore::clientFilePath(const std::string& name) const
{
    // Callers should already have validated `name` via isSafeName before
    // reaching here; the path-traversal guard is mirrored in issueToken /
    // revokeToken. Anything that slips through still gets a path that's
    // textually under m_tokensDir, but not normalised — defensive check
    // below in writeClientFile keeps us inside the directory.
    return m_tokensDir + "/" + name + ".json";
}

bool TokenStore::writeClientFile(const std::string& name,
                                 const std::string& token,
                                 const std::string& endpointPath) const
{
    if (!isSafeName(name)) return false;

    json j;
    j["name"] = name;
    j["token"] = token;
    j["endpoint"] = endpointPath;

    std::error_code ec;
    fs::create_directories(m_tokensDir, ec);
    if (ec) return false;

    // Belt-and-braces: even if isSafeName() somehow lets a traversal
    // through, weldcanonical paths and refuse to write outside the
    // tokens dir. Use weakly_canonical because the target file may not
    // exist yet at this point.
    const fs::path target = fs::weakly_canonical(fs::path(clientFilePath(name)));
    const fs::path root   = fs::weakly_canonical(fs::path(m_tokensDir));
    const auto rel = fs::relative(target, root, ec);
    if (ec || rel.empty() || *rel.begin() == "..") return false;

    const std::string path = clientFilePath(name);
    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return false;
    ofs << j.dump(4) << "\n";
    if (!ofs.good()) return false;
    ofs.close();
    // Tokens are credentials; force restrictive perms regardless of
    // umask so we never accidentally create world/group-readable
    // credential files. POSIX-only — fine, the daemon doesn't run on
    // Windows.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
    return true;
}

// ── Persistence ────────────────────────────────────────────────────────────

std::vector<std::pair<std::string, TokenStore::Entry>>
TokenStore::loadDb() const
{
    std::vector<std::pair<std::string, Entry>> out;
    std::ifstream ifs(m_dbPath);
    if (!ifs) return out;

    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return out; }
    if (!obj.is_object()) return out;

    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it.value().is_object()) continue;
        Entry e;
        e.name     = it.value().value("name", std::string{});
        e.issuedAt = it.value().value("issued_at", std::string{});
        out.emplace_back(it.key(), std::move(e));
    }
    return out;
}

bool TokenStore::saveDb(
    const std::vector<std::pair<std::string, Entry>>& db) const
{
    json obj = json::object();
    for (const auto& [hash, e] : db) {
        obj[hash] = { {"name", e.name}, {"issued_at", e.issuedAt} };
    }
    std::error_code ec;
    fs::create_directories(fs::path(m_dbPath).parent_path(), ec);
    std::ofstream ofs(m_dbPath, std::ios::trunc);
    if (!ofs) return false;
    ofs << obj.dump(4) << "\n";
    if (!ofs.good()) return false;
    ofs.close();
    // tokens.db carries the digest of every active token — set 0600
    // so umask can't widen it accidentally.
    ::chmod(m_dbPath.c_str(), S_IRUSR | S_IWUSR);
    return true;
}
