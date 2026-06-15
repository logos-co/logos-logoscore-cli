#include <gtest/gtest.h>

#include "daemon/token_store.h"
#include "daemon/daemon_state.h"
#include "config.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

class TokenStoreTest : public ::testing::Test {
protected:
    fs::path dir;
    std::string origConfigDir;

    void SetUp() override {
        dir = fs::temp_directory_path() / ("tokenstore-" + std::to_string(::getpid()) +
                                           "-" + std::to_string(rand()));
        fs::create_directories(dir);
        // Route TokensFile (which TokenStore persists into) at our
        // temp dir. TokenStore's ctor takes a configDir but the
        // underlying file uses Config::daemonTokensPath().
        const char* cd = std::getenv("LOGOSCORE_CONFIG_DIR");
        origConfigDir = cd ? cd : "";
        setenv("LOGOSCORE_CONFIG_DIR", dir.string().c_str(), 1);
        Config::setConfigDir("");
    }
    void TearDown() override {
        if (origConfigDir.empty()) unsetenv("LOGOSCORE_CONFIG_DIR");
        else setenv("LOGOSCORE_CONFIG_DIR", origConfigDir.c_str(), 1);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    TokenStore makeStore() { return TokenStore(); }

    // Test helper: assert issue succeeded and return the raw token.
    // Most tests don't care about the new error-code surface; they
    // want "give me a token" semantics.
    static std::string mustIssue(
        TokenStore& store,
        const std::string& name,
        const std::string& expiresAt = {},
        bool localOnly = false,
        bool replace = false)
    {
        auto r = store.issueToken(name, expiresAt, localOnly, replace);
        EXPECT_EQ(r.status, TokenStore::IssueStatus::Ok)
            << "expected Ok issuing token '" << name
            << "', got status=" << static_cast<int>(r.status);
        return r.token;
    }
};

TEST_F(TokenStoreTest, IssueThenLookupByToken)
{
    TokenStore store = makeStore();
    auto r = store.issueToken("alice");
    ASSERT_EQ(r.status, TokenStore::IssueStatus::Ok);
    EXPECT_FALSE(r.token.empty());

    auto who = store.lookupByToken(r.token, "local");
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(*who, "alice");

    EXPECT_FALSE(store.lookupByToken("bogus", "local").has_value());
}

TEST_F(TokenStoreTest, IssueDuplicateFailsWithoutReplace)
{
    TokenStore store = makeStore();
    EXPECT_EQ(store.issueToken("alice").status, TokenStore::IssueStatus::Ok);
    EXPECT_EQ(store.issueToken("alice").status,
              TokenStore::IssueStatus::AlreadyExists);
}

TEST_F(TokenStoreTest, Issue_InvalidNameRejected)
{
    TokenStore store = makeStore();
    // Path-traversal attempt — must surface as InvalidName, not
    // AlreadyExists / IoError.
    EXPECT_EQ(store.issueToken("../etc/passwd").status,
              TokenStore::IssueStatus::InvalidName);
    EXPECT_EQ(store.issueToken("").status,
              TokenStore::IssueStatus::InvalidName);
}

TEST_F(TokenStoreTest, IssueReplaceRotatesToken)
{
    TokenStore store = makeStore();
    const auto first = mustIssue(store, "alice");
    auto second = store.issueToken("alice", /*expiresAt=*/{},
                                   /*localOnly=*/false, /*replace=*/true);
    ASSERT_EQ(second.status, TokenStore::IssueStatus::Ok);
    EXPECT_NE(first, second.token);

    EXPECT_FALSE(store.lookupByToken(first, "local").has_value());
    EXPECT_EQ(store.lookupByToken(second.token, "local").value(), "alice");
}

TEST_F(TokenStoreTest, Revoke)
{
    TokenStore store = makeStore();
    const auto tok = mustIssue(store, "bob");
    EXPECT_EQ(store.revokeToken("bob"), TokenStore::RevokeStatus::Ok);
    EXPECT_FALSE(store.lookupByToken(tok, "local").has_value());
    EXPECT_EQ(store.revokeToken("bob"), TokenStore::RevokeStatus::NotFound);
}

TEST_F(TokenStoreTest, Revoke_InvalidNameRejected)
{
    TokenStore store = makeStore();
    EXPECT_EQ(store.revokeToken("../etc/passwd"),
              TokenStore::RevokeStatus::InvalidName);
}

TEST_F(TokenStoreTest, ListIncludesEveryIssuedName)
{
    TokenStore store = makeStore();
    mustIssue(store, "alice");
    mustIssue(store, "bob");
    mustIssue(store, "carol");

    auto entries = store.listTokens();
    std::vector<std::string> names;
    for (auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"alice", "bob", "carol"}));
}

TEST_F(TokenStoreTest, RawTokenFile_ContainsRawToken)
{
    TokenStore store = makeStore();
    const auto tok = mustIssue(store, "alice");
    const auto path = store.rawTokenFilePath("alice");
    std::ifstream ifs(path);
    ASSERT_TRUE(ifs) << "expected raw token file at " << path;
    std::string contents((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find(tok), std::string::npos);
    EXPECT_NE(contents.find("\"alice\""), std::string::npos);
}

TEST_F(TokenStoreTest, RawFileMissing_ValidationStillSucceeds)
{
    // After the operator copies the raw file to a client and rm's it on
    // the daemon side, validation must continue to work — the hash
    // entry in tokens.json is what authenticates from then on.
    TokenStore store = makeStore();
    const auto tok = mustIssue(store, "alice");
    const auto raw = store.rawTokenFilePath("alice");
    ASSERT_TRUE(fs::exists(raw));
    fs::remove(raw);
    auto who = store.lookupByToken(tok, "local");
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(*who, "alice");
}

TEST_F(TokenStoreTest, PersistenceRoundTrip)
{
    std::string tok;
    {
        TokenStore store = makeStore();
        tok = mustIssue(store, "alice");
    }
    TokenStore store2 = makeStore();
    auto who = store2.lookupByToken(tok, "local");
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(*who, "alice");
}

TEST_F(TokenStoreTest, HashIsStable)
{
    EXPECT_EQ(TokenStore::hashToken("hello"),
              TokenStore::hashToken("hello"));
    EXPECT_NE(TokenStore::hashToken("hello"),
              TokenStore::hashToken("world"));
}

// Security checks for the new validation contract.

TEST_F(TokenStoreTest, LocalOnly_RejectedOverNonLocal)
{
    TokenStore store = makeStore();
    const auto tok = mustIssue(store, "auto", /*expiresAt=*/{}, /*localOnly=*/true);

    EXPECT_TRUE(store.lookupByToken(tok, "local").has_value());
    EXPECT_FALSE(store.lookupByToken(tok, "tcp").has_value());
    EXPECT_FALSE(store.lookupByToken(tok, "tcp_ssl").has_value());
}

TEST_F(TokenStoreTest, NotLocalOnly_AcceptedOverEveryTransport)
{
    TokenStore store = makeStore();
    const auto tok = mustIssue(store, "alice");

    EXPECT_TRUE(store.lookupByToken(tok, "local").has_value());
    EXPECT_TRUE(store.lookupByToken(tok, "tcp").has_value());
    EXPECT_TRUE(store.lookupByToken(tok, "tcp_ssl").has_value());
}

TEST_F(TokenStoreTest, Expired_Rejected)
{
    TokenStore store = makeStore();
    // 1-second deadline in the past.
    const auto pastDeadline = std::string("2000-01-01T00:00:00Z");
    const auto tok = mustIssue(store, "bob", pastDeadline);
    EXPECT_FALSE(store.lookupByToken(tok, "local").has_value());
}

TEST_F(TokenStoreTest, NotExpired_Accepted)
{
    TokenStore store = makeStore();
    // Far-future deadline.
    const auto futureDeadline = std::string("2099-12-31T23:59:59Z");
    const auto tok = mustIssue(store, "bob", futureDeadline);
    EXPECT_TRUE(store.lookupByToken(tok, "local").has_value());
}

TEST_F(TokenStoreTest, ExpiresAt_MalformedFailsClosed)
{
    // Hand-edit / corruption / partial-write of tokens.json that
    // leaves expires_at unparseable. The lookup path must reject the
    // entry rather than silently treat it as non-expiring (fail
    // closed). Issue a fresh token with a valid expiry, then patch
    // tokens.json on disk to a bad value and re-read.
    TokenStore store = makeStore();
    const auto tok = mustIssue(store, "dave",
        std::string("2099-01-01T00:00:00Z"));
    ASSERT_TRUE(store.lookupByToken(tok, "local").has_value());

    auto entries = TokensFile::read();
    ASSERT_EQ(entries.size(), 1u);
    entries[0].expiresAt = "not-a-real-date";
    ASSERT_TRUE(TokensFile::write(entries));

    EXPECT_FALSE(store.lookupByToken(tok, "local").has_value())
        << "malformed expires_at must reject the token, not pass through "
           "as non-expiring";
}

// -- BUG-011: empty token must never authenticate -------------------------
//
// generateToken() returns "" when RAND_bytes fails. issueToken() did not
// check for that, so it would persist an entry whose hash is SHA-256("")
// and a corresponding empty raw-token file, and report success. A client
// sending an empty token would then hash to the same SHA-256("") and match
// the entry. lookupByToken must fail closed on an empty token regardless of
// what is on disk. We simulate the corrupt on-disk state directly (an entry
// whose hash == hashToken("")), since we can't force RAND_bytes to fail.
TEST_F(TokenStoreTest, EmptyToken_NeverAuthenticates)
{
    TokenStore store = makeStore();

    // Forge the exact entry a failed-RNG issue would have written:
    // hash of the empty string, no expiry, not local-only.
    TokenEntry bogus;
    bogus.name     = "auto";
    bogus.hash     = TokenStore::hashToken("");   // SHA-256 of ""
    bogus.issuedAt = "2024-01-01T00:00:00Z";
    ASSERT_TRUE(TokensFile::write({bogus}));

    // An inbound empty token hashes to the same value and would match the
    // forged entry — but auth must reject it outright.
    EXPECT_FALSE(store.lookupByToken("", "local").has_value())
        << "an empty token must never authenticate, even if a corrupt "
           "empty-hash entry exists in tokens.json";
}

// -- BUG-011 (issue side): a failed-RNG empty token must not be persisted --
//
// We can't force RAND_bytes to fail, but we can assert the contract that
// issueToken never reports Ok while handing back an empty raw token. Today
// the only way to exercise the empty-token branch is RNG failure; this test
// documents and locks the invariant for the normal path (token non-empty on
// Ok) so a regression that drops the guard is caught.
TEST_F(TokenStoreTest, IssueOk_ImpliesNonEmptyToken)
{
    TokenStore store = makeStore();
    auto r = store.issueToken("alice");
    ASSERT_EQ(r.status, TokenStore::IssueStatus::Ok);
    EXPECT_FALSE(r.token.empty())
        << "issueToken must never return Ok with an empty token";
}

// -- BUG-025: a failed --replace must not destroy the prior raw token -----
//
// On --replace the old code overwrote the operator-visible raw file FIRST,
// then wrote tokens.json; if the tokens.json write failed it removed the raw
// file in "rollback" — destroying the only on-disk copy of a token whose
// hash (unchanged on disk) was still valid. We force the tokens.json write
// to fail by making the daemon/ dir read-only (so the temp+rename in that
// dir can't be created), while the tokens/ subdir stays writable so the raw
// write itself can proceed. After the failed replace the original raw file
// must still be present and unchanged.
TEST_F(TokenStoreTest, ReplaceWriteFailure_PreservesPriorRawToken)
{
    TokenStore store = makeStore();
    const auto first = mustIssue(store, "alice");
    const auto rawPath = store.rawTokenFilePath("alice");
    ASSERT_TRUE(fs::exists(rawPath));
    std::string before((std::istreambuf_iterator<char>(
                            *std::make_unique<std::ifstream>(rawPath))),
                        std::istreambuf_iterator<char>());
    ASSERT_NE(before.find(first), std::string::npos);

    // Make daemon/ (which holds tokens.json + its .tmp) read-only so the
    // tokens.json write fails; tokens/ keeps its own 0700 and stays writable.
    const fs::path daemonDir = fs::path(Config::daemonTokensPath()).parent_path();
    ::chmod(daemonDir.c_str(), 0500);

    auto r = store.issueToken("alice", /*expiresAt=*/{},
                              /*localOnly=*/false, /*replace=*/true);

    ::chmod(daemonDir.c_str(), 0700);  // restore for assertions/teardown

    EXPECT_EQ(r.status, TokenStore::IssueStatus::IoError)
        << "a tokens.json write failure during --replace must report IoError";

    ASSERT_TRUE(fs::exists(rawPath))
        << "the prior raw token file must survive a failed --replace";
    std::string after((std::istreambuf_iterator<char>(
                           *std::make_unique<std::ifstream>(rawPath))),
                      std::istreambuf_iterator<char>());
    EXPECT_NE(after.find(first), std::string::npos)
        << "the surviving raw file must still contain the original token";
    // And the original token must still validate (its hash never changed).
    EXPECT_EQ(store.lookupByToken(first, "local").value_or(""), "alice");
}

// -- BUG-024: issuing must not silently clobber an unsupported-version file
//
// TokensFile::read() returns an empty vector for a tokens.json whose schema
// version it doesn't recognise (logging to stderr). issueToken then appended
// to that empty list and rewrote the file at the CURRENT version —
// destroying every operator-issued token that lived in the unrecognised
// file. issueToken/revokeToken must instead refuse (IoError) and leave the
// on-disk file byte-for-byte intact, so an operator who is mid-migration can
// recover their tokens instead of finding them silently wiped.
TEST_F(TokenStoreTest, IssueRefusesToClobberUnsupportedVersionFile)
{
    // Hand-write a tokens.json one version ahead, carrying a real entry.
    const std::string path = TokensFile::filePath();
    fs::create_directories(fs::path(path).parent_path());
    const std::string futureDoc =
        std::string("{\n  \"version\": ") +
        std::to_string(kTokensFileSchemaVersion + 1) +
        ",\n  \"tokens\": [\n"
        "    { \"name\": \"alice\", \"hash\": \"deadbeef\", "
        "\"issued_at\": \"2024-01-01T00:00:00Z\", \"expires_at\": null, "
        "\"local_only\": false }\n  ]\n}\n";
    {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << futureDoc;
    }

    TokenStore store = makeStore();
    auto r = store.issueToken("bob");
    EXPECT_EQ(r.status, TokenStore::IssueStatus::IoError)
        << "issuing against an unsupported-version tokens.json must refuse, "
           "not silently rewrite it at the current version";

    // The on-disk file must be untouched (still the future version + alice).
    std::string after((std::istreambuf_iterator<char>(
                           *std::make_unique<std::ifstream>(path))),
                      std::istreambuf_iterator<char>());
    EXPECT_NE(after.find("\"version\": " +
                         std::to_string(kTokensFileSchemaVersion + 1)),
              std::string::npos)
        << "the unsupported-version file must be left intact, not clobbered";
    EXPECT_NE(after.find("alice"), std::string::npos)
        << "the operator's existing token entry must survive";
}

// -- TokensFile direct round-trip -----------------------------------------
//
// Verifies the standalone tokens.json shape: read returns an empty
// vector when the file is missing, write+read preserves every field
// (including the null-vs-set expires_at distinction), and reading a
// file with a wrong schema version yields an empty vector rather
// than crashing or returning malformed entries.

TEST_F(TokenStoreTest, TokensFile_MissingReturnsEmpty)
{
    EXPECT_TRUE(TokensFile::read().empty());
}

TEST_F(TokenStoreTest, TokensFile_RoundTripPreservesEveryField)
{
    std::vector<TokenEntry> in;
    in.push_back({"auto",  "deadbeef", "2026-04-28T00:00:00Z", "",                       true});
    in.push_back({"alice", "abc123",   "2026-04-28T01:00:00Z", "2027-01-01T00:00:00Z",  false});
    ASSERT_TRUE(TokensFile::write(in));

    auto out = TokensFile::read();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].name, "auto");
    EXPECT_EQ(out[0].hash, "deadbeef");
    EXPECT_TRUE(out[0].expiresAt.empty());
    EXPECT_TRUE(out[0].localOnly);
    EXPECT_EQ(out[1].name, "alice");
    EXPECT_EQ(out[1].expiresAt, "2027-01-01T00:00:00Z");
    EXPECT_FALSE(out[1].localOnly);
}

TEST_F(TokenStoreTest, TokensFile_RejectsUnknownSchemaVersion)
{
    // Hand-write a tokens.json with a version we don't accept; the
    // reader must not crash and must return zero entries (so a
    // running daemon's accepted-token set effectively becomes empty
    // and the operator gets a clear stderr message).
    fs::path p(TokensFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p) << R"({"version":99,"tokens":[]})" << "\n";
    EXPECT_TRUE(TokensFile::read().empty());
}
