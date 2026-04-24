#include "token_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

} // anonymous namespace

// ── hashToken ──────────────────────────────────────────────────────────────
//
// Non-cryptographic FNV-1a 64-bit, used only as a deterministic keying
// function for the tokens.db map — not as a security measure. The real
// protection against disk exfiltration is file-system permissions on
// $CONFIG_DIR (mode 0700 dir, 0600 files), same as today's daemon.json.
// A future iteration can swap this for a proper keyed MAC once we have
// a place to store the daemon's MAC key.
std::string TokenStore::hashToken(const std::string& token)
{
    constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
    constexpr uint64_t kFnvPrime  = 0x100000001b3ULL;
    uint64_t h = kFnvOffset;
    for (unsigned char c : token) {
        h ^= c;
        h *= kFnvPrime;
    }
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << h;
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
    if (name.empty()) return std::nullopt;

    auto db = loadDb();
    for (auto& [hash, e] : db) {
        if (e.name == name) {
            if (!replace) return std::nullopt;
            // replace: drop this entry, will add new below.
            db.erase(std::remove_if(db.begin(), db.end(),
                                    [&](const auto& kv) { return kv.second.name == name; }),
                     db.end());
            break;
        }
    }

    std::string token = generateToken();
    Entry e{name, utcIso8601()};
    db.emplace_back(hashToken(token), std::move(e));

    if (!saveDb(db)) return std::nullopt;
    return token;
}

bool TokenStore::revokeToken(const std::string& name)
{
    auto db = loadDb();
    const auto before = db.size();
    db.erase(std::remove_if(db.begin(), db.end(),
                            [&](const auto& kv) { return kv.second.name == name; }),
             db.end());
    if (db.size() == before) return false;
    saveDb(db);

    // Also best-effort remove the client file (not fatal on failure).
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
    return m_tokensDir + "/" + name + ".json";
}

bool TokenStore::writeClientFile(const std::string& name,
                                 const std::string& token,
                                 const std::string& endpointPath) const
{
    json j;
    j["name"] = name;
    j["token"] = token;
    j["endpoint"] = endpointPath;

    std::error_code ec;
    fs::create_directories(m_tokensDir, ec);
    if (ec) return false;

    std::ofstream ofs(clientFilePath(name), std::ios::trunc);
    if (!ofs) return false;
    ofs << j.dump(4) << "\n";
    return ofs.good();
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
    return ofs.good();
}
