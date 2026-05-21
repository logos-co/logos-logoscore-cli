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
