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
        // Route DaemonStateFile (which TokenStore now persists into) at
        // our temp dir. TokenStore's own ctor takes a configDir but the
        // underlying state file uses Config::daemonConfigPath().
        origConfigDir = qEnvironmentVariable("LOGOSCORE_CONFIG_DIR").toStdString();
        qputenv("LOGOSCORE_CONFIG_DIR", dir.string().c_str());
        Config::setConfigDir(QString());
    }
    void TearDown() override {
        if (origConfigDir.empty()) qunsetenv("LOGOSCORE_CONFIG_DIR");
        else qputenv("LOGOSCORE_CONFIG_DIR", origConfigDir.c_str());
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    TokenStore makeStore() { return TokenStore(dir.string()); }
};

TEST_F(TokenStoreTest, IssueThenLookupByToken)
{
    TokenStore store = makeStore();
    auto t = store.issueToken("alice");
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->empty());

    auto who = store.lookupByToken(*t, "local");
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(*who, "alice");

    EXPECT_FALSE(store.lookupByToken("bogus", "local").has_value());
}

TEST_F(TokenStoreTest, IssueDuplicateFailsWithoutReplace)
{
    TokenStore store = makeStore();
    ASSERT_TRUE(store.issueToken("alice").has_value());
    EXPECT_FALSE(store.issueToken("alice").has_value());
}

TEST_F(TokenStoreTest, IssueReplaceRotatesToken)
{
    TokenStore store = makeStore();
    const auto first = store.issueToken("alice").value();
    auto second = store.issueToken("alice", /*expiresAt=*/{}, /*localOnly=*/false, /*replace=*/true);
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first, *second);

    EXPECT_FALSE(store.lookupByToken(first, "local").has_value());
    EXPECT_EQ(store.lookupByToken(*second, "local").value(), "alice");
}

TEST_F(TokenStoreTest, Revoke)
{
    TokenStore store = makeStore();
    const auto tok = store.issueToken("bob").value();
    EXPECT_TRUE(store.revokeToken("bob"));
    EXPECT_FALSE(store.lookupByToken(tok, "local").has_value());
    EXPECT_FALSE(store.revokeToken("bob"));
}

TEST_F(TokenStoreTest, ListIncludesEveryIssuedName)
{
    TokenStore store = makeStore();
    store.issueToken("alice");
    store.issueToken("bob");
    store.issueToken("carol");

    auto entries = store.listTokens();
    std::vector<std::string> names;
    for (auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"alice", "bob", "carol"}));
}

TEST_F(TokenStoreTest, RawTokenFile_ContainsRawToken)
{
    TokenStore store = makeStore();
    const auto tok = store.issueToken("alice").value();
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
    // entry in daemon.json is what authenticates from then on.
    TokenStore store = makeStore();
    const auto tok = store.issueToken("alice").value();
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
        tok = store.issueToken("alice").value();
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
    const auto tok = store.issueToken("auto", /*expiresAt=*/{}, /*localOnly=*/true).value();

    EXPECT_TRUE(store.lookupByToken(tok, "local").has_value());
    EXPECT_FALSE(store.lookupByToken(tok, "tcp").has_value());
    EXPECT_FALSE(store.lookupByToken(tok, "tcp_ssl").has_value());
}

TEST_F(TokenStoreTest, NotLocalOnly_AcceptedOverEveryTransport)
{
    TokenStore store = makeStore();
    const auto tok = store.issueToken("alice").value();

    EXPECT_TRUE(store.lookupByToken(tok, "local").has_value());
    EXPECT_TRUE(store.lookupByToken(tok, "tcp").has_value());
    EXPECT_TRUE(store.lookupByToken(tok, "tcp_ssl").has_value());
}

TEST_F(TokenStoreTest, Expired_Rejected)
{
    TokenStore store = makeStore();
    // 1-second deadline in the past.
    const auto pastDeadline = std::string("2000-01-01T00:00:00Z");
    const auto tok = store.issueToken("bob", pastDeadline).value();
    EXPECT_FALSE(store.lookupByToken(tok, "local").has_value());
}

TEST_F(TokenStoreTest, NotExpired_Accepted)
{
    TokenStore store = makeStore();
    // Far-future deadline.
    const auto futureDeadline = std::string("2099-12-31T23:59:59Z");
    const auto tok = store.issueToken("bob", futureDeadline).value();
    EXPECT_TRUE(store.lookupByToken(tok, "local").has_value());
}
