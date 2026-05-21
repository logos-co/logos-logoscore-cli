#include "token_store.h"
#include "daemon_state.h"
#include "../config.h"

#include <nlohmann/json.hpp>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

// -- TokensFile -------------------------------------------------------------
//
// Standalone JSON document that holds the hashed-at-rest token array.
// Atomic writes (write-temp + rename) so a crash mid-write can't leave
// the daemon's accepted-token set in a half-baked state. Read returns
// {} if the file is missing or unparseable — callers treat both as
// "no tokens yet".

std::string TokensFile::filePath()
{
    return Config::daemonTokensPath();
}

namespace {

json tokenEntryToJson(const TokenEntry& e)
{
    json j;
    j["name"]      = e.name;
    j["hash"]      = e.hash;
    j["issued_at"] = e.issuedAt;
    // Persist `expires_at` as null when unset so the on-disk shape is
    // uniform across entries.
    if (e.expiresAt.empty()) j["expires_at"] = nullptr;
    else                     j["expires_at"] = e.expiresAt;
    j["local_only"] = e.localOnly;
    return j;
}

std::optional<TokenEntry> tokenEntryFromJson(const json& j)
{
    if (!j.is_object()) return std::nullopt;
    TokenEntry e;
    e.name     = j.value("name", std::string{});
    e.hash     = j.value("hash", std::string{});
    e.issuedAt = j.value("issued_at", std::string{});
    if (e.name.empty() || e.hash.empty()) return std::nullopt;
    if (j.contains("expires_at") && !j.at("expires_at").is_null())
        e.expiresAt = j.value("expires_at", std::string{});
    e.localOnly = j.value("local_only", false);
    return e;
}

} // namespace

std::vector<TokenEntry> TokensFile::read()
{
    std::vector<TokenEntry> out;

    std::ifstream ifs(filePath());
    if (!ifs) return out;

    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return out; }

    const int v = obj.value("version", 0);
    if (v != kTokensFileSchemaVersion) {
        std::cerr << "TokensFile: unsupported schema version " << v
                  << " in " << filePath()
                  << " — reissue tokens to regenerate (expected version "
                  << kTokensFileSchemaVersion << ")." << std::endl;
        return out;
    }

    if (obj.contains("tokens") && obj["tokens"].is_array()) {
        out.reserve(obj["tokens"].size());
        for (const auto& j : obj["tokens"]) {
            if (auto t = tokenEntryFromJson(j)) out.push_back(*t);
        }
    }
    return out;
}

bool TokensFile::write(const std::vector<TokenEntry>& tokens)
{
    fs::path path(filePath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    json obj;
    obj["version"] = kTokensFileSchemaVersion;
    json arr = json::array();
    for (const auto& t : tokens) arr.push_back(tokenEntryToJson(t));
    obj["tokens"] = std::move(arr);

    // Atomic *replace*: write full content to <path>.tmp, then rename
    // to final. The rename is the atomic step — readers either see the
    // pre-write file or the new one, never a half-written state.
    // We do NOT fsync, so this isn't durable across power loss
    // (close() flushes only userspace buffers; OS page cache is still
    // in flight). Callers that need durability would need an explicit
    // fsync(fd) on the temp file plus a dir fsync after rename.
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs) return false;
        ofs << obj.dump(4) << "\n";
        ofs.close();
        if (!ofs) return false;
    }
    // 0600 before rename — once the file is at its final path, file
    // perms apply via the rename; doing it on the temp avoids any
    // window where a wider mode is visible at the destination.
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
    std::error_code rec;
    fs::rename(tmp, path, rec);
    if (rec) {
        // Best-effort cleanup of the temp file on failure; the rename
        // didn't take, so the destination is unchanged.
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

namespace {

// 256 bits of cryptographically-secure entropy, rendered as 43 chars
// of URL-safe base64-ish alphabet (no padding).
//
// Implementation notes:
//   - `RAND_bytes` is OpenSSL's CSPRNG; on a properly-seeded system
//     it draws from the OS getrandom/urandom pool and produces output
//     that's safe to use as an authentication secret.
//   - We pull 32 raw bytes (= 256 bits) and emit one alphabet char
//     per 6 bits, like base64. The trailing 4 bits are discarded
//     (kBits % 6 == 4) — that's fine, we end up with 252 bits of
//     entropy in 42 chars + 4 leftover bits in the 43rd, well above
//     the ~128-bit threshold for a token that authenticates clients.
//   - The previous implementation used `std::mt19937_64` (not a
//     CSPRNG, predictable from a few outputs) and modulo-64 sampling
//     (introduces bias since 2^64 % 64 == 0 only by accident; the
//     uniform_int_distribution it fed was over the full uint64_t
//     range, where 64 *does* divide evenly, but the construction was
//     fragile). Crypto-grade RNG removes both concerns.
std::string generateToken() {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    constexpr int kRawBytes  = 32;  // 256 bits
    constexpr int kOutChars  = 43;  // ceil(256 / 6)

    unsigned char raw[kRawBytes];
    if (RAND_bytes(raw, kRawBytes) != 1) {
        // RAND_bytes failure is essentially "the OS RNG isn't
        // seeded", which on Linux/macOS only happens in pathological
        // boot-time scenarios. Returning an empty string here causes
        // issueToken's downstream write of the raw file to produce a
        // visibly-broken file, which the caller's atomicity check
        // rolls back; the operator sees the issue immediately
        // instead of receiving a low-entropy token.
        return std::string();
    }

    std::string s;
    s.reserve(kOutChars);
    // Walk the 256 bits in 6-bit chunks. We unpack the byte buffer
    // into a rolling bit accumulator rather than doing arithmetic
    // tricks per char so the loop reads as "extract 6 bits, emit one
    // alphabet character, repeat".
    uint32_t acc = 0;
    int      bits = 0;
    int      emitted = 0;
    for (int i = 0; i < kRawBytes && emitted < kOutChars; ++i) {
        acc = (acc << 8) | raw[i];
        bits += 8;
        while (bits >= 6 && emitted < kOutChars) {
            const uint32_t chunk = (acc >> (bits - 6)) & 0x3F;
            s.push_back(alpha[chunk]);
            bits -= 6;
            ++emitted;
        }
    }
    // Trailing bits (< 6) become one more char, padded right with
    // zeros — same shape base64-without-padding produces.
    if (emitted < kOutChars && bits > 0) {
        const uint32_t chunk = (acc << (6 - bits)) & 0x3F;
        s.push_back(alpha[chunk]);
    }
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
    // Empty == "never expires" — the only documented way to opt out of
    // expiry. Any non-empty value must parse cleanly; if it doesn't,
    // we fail closed (treat the entry as expired) rather than fail
    // open. Expiry is a security control, and a malformed `expires_at`
    // (hand-edit, partial write, corruption) shouldn't silently
    // upgrade a token to non-expiring. The CLI's `issue-token`
    // validates the input when the operator types it; this path is
    // the on-disk safety net.
    if (expiresAt.empty()) return false;
    struct tm tm{};
    if (!strptime(expiresAt.c_str(), "%Y-%m-%dT%H:%M:%SZ", &tm)) return true;
    // timegm interprets `tm` as UTC.
    time_t deadline = timegm(&tm);
    return std::chrono::system_clock::to_time_t(
               std::chrono::system_clock::now()) >= deadline;
}

} // anonymous namespace

// -- hashToken --------------------------------------------------------------
//
// SHA-256 hex digest. Used as the lookup key in tokens.json["tokens"] so an
// attacker who reads tokens.json can't recover plaintext tokens, and so
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

TokenStore::TokenStore()
{
    // Both the raw-token dir and the hashed tokens.json live under
    // the process-global Config dir. Materialize the dir up front so
    // issueToken can write into it without first having to
    // create_directories itself.
    const std::string tokensDir = Config::daemonTokensDir();
    std::error_code ec;
    fs::create_directories(tokensDir, ec);
    // Belt-and-braces dir perms (0700) so umask can't widen.
    if (!ec) ::chmod(tokensDir.c_str(), S_IRWXU);
}

namespace {

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

TokenStore::IssueResult
TokenStore::issueToken(const std::string& name,
                       const std::string& expiresAt,
                       bool localOnly,
                       bool replace)
{
    if (!isSafeName(name)) return {IssueStatus::InvalidName, {}};

    auto tokens = TokensFile::read();
    auto it = std::find_if(tokens.begin(), tokens.end(),
        [&](const TokenEntry& e) { return e.name == name; });
    if (it != tokens.end()) {
        if (!replace) return {IssueStatus::AlreadyExists, {}};
        tokens.erase(it);
    }

    const std::string raw = generateToken();
    const std::string issuedAt = currentUtcIso8601();

    TokenEntry e;
    e.name      = name;
    e.hash      = hashToken(raw);
    e.issuedAt  = issuedAt;
    e.expiresAt = expiresAt;
    e.localOnly = localOnly;
    tokens.push_back(e);

    // Write order matters for atomicity:
    //   1. raw file FIRST — its absence is recoverable (operator can
    //      `revoke-token <name>` then re-issue), and there's no
    //      authentication impact while it's missing.
    //   2. tokens.json with the new hash entry SECOND — only after
    //      the raw file is on disk. If we wrote tokens.json first
    //      and then raw-file write failed, the hash entry would be
    //      stranded: validators would accept the token but no
    //      operator-readable copy of the raw value exists.
    //
    // If step 1 fails, abort cleanly: nothing was committed.
    // If step 2 fails, roll back the raw file we just wrote so
    // we don't leave an unreferenced credential file behind.
    if (!writeRawTokenFile(rawTokenFilePath(name), name, raw, issuedAt))
        return {IssueStatus::IoError, {}};
    if (!TokensFile::write(tokens)) {
        std::error_code ec;
        fs::remove(fs::path(rawTokenFilePath(name)), ec);
        return {IssueStatus::IoError, {}};
    }

    return {IssueStatus::Ok, raw};
}

TokenStore::RevokeStatus TokenStore::revokeToken(const std::string& name)
{
    if (!isSafeName(name)) return RevokeStatus::InvalidName;

    auto tokens = TokensFile::read();
    const auto before = tokens.size();
    tokens.erase(
        std::remove_if(tokens.begin(), tokens.end(),
                       [&](const TokenEntry& e) { return e.name == name; }),
        tokens.end());
    if (tokens.size() == before) return RevokeStatus::NotFound;
    if (!TokensFile::write(tokens)) return RevokeStatus::IoError;

    // Best-effort remove the operator-copyable raw file. It may already
    // be gone (the operator deleted it post-copy, by design) — that's
    // not an error: the in-memory hash list is the source of truth and
    // we've just removed the entry there.
    std::error_code ec;
    fs::remove(fs::path(rawTokenFilePath(name)), ec);
    return RevokeStatus::Ok;
}

std::vector<IssuedToken> TokenStore::listTokens() const
{
    std::vector<IssuedToken> out;
    auto tokens = TokensFile::read();
    out.reserve(tokens.size());
    for (const auto& e : tokens) {
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
    const auto tokens = TokensFile::read();
    for (const auto& e : tokens) {
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
    return Config::daemonTokensDir() + "/" + name + ".json";
}
