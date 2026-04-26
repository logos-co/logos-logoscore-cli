#include <gtest/gtest.h>

#include "daemon/token_store.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class TokenStoreTest : public ::testing::Test {
protected:
    fs::path dir;

    void SetUp() override {
        dir = fs::temp_directory_path() / ("tokenstore-" + std::to_string(::getpid()) +
                                           "-" + std::to_string(rand()));
        fs::create_directories(dir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

TEST_F(TokenStoreTest, IssueThenLookupByToken)
{
    TokenStore store(dir.string());
    auto t = store.issueToken("alice");
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->empty());

    auto who = store.lookupByToken(*t);
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(*who, "alice");

    EXPECT_FALSE(store.lookupByToken("bogus").has_value());
}

TEST_F(TokenStoreTest, IssueDuplicateFailsWithoutReplace)
{
    TokenStore store(dir.string());
    auto first = store.issueToken("alice");
    ASSERT_TRUE(first.has_value());
    auto second = store.issueToken("alice");
    EXPECT_FALSE(second.has_value());
}

TEST_F(TokenStoreTest, IssueReplaceRotatesToken)
{
    TokenStore store(dir.string());
    auto first = store.issueToken("alice").value();
    auto second = store.issueToken("alice", /*replace=*/true);
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first, *second);

    // Only the new one validates.
    EXPECT_FALSE(store.lookupByToken(first).has_value());
    EXPECT_EQ(store.lookupByToken(*second).value(), "alice");
}

TEST_F(TokenStoreTest, Revoke)
{
    TokenStore store(dir.string());
    auto tok = store.issueToken("bob").value();
    EXPECT_TRUE(store.revokeToken("bob"));
    EXPECT_FALSE(store.lookupByToken(tok).has_value());
    EXPECT_FALSE(store.revokeToken("bob"));  // gone now
}

TEST_F(TokenStoreTest, ListIncludesEveryIssuedName)
{
    TokenStore store(dir.string());
    store.issueToken("alice");
    store.issueToken("bob");
    store.issueToken("carol");

    auto entries = store.listTokens();
    std::vector<std::string> names;
    for (auto& e : entries) names.push_back(e.name);
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names, (std::vector<std::string>{"alice", "bob", "carol"}));
}

TEST_F(TokenStoreTest, ClientFileContainsRawToken)
{
    TokenStore store(dir.string());
    auto tok = store.issueToken("alice").value();
    ASSERT_TRUE(store.writeClientFile("alice", tok));

    const auto path = store.clientFilePath("alice");
    std::ifstream ifs(path);
    ASSERT_TRUE(ifs) << "expected client file at " << path;
    std::string contents((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find(tok), std::string::npos);
    EXPECT_NE(contents.find("\"alice\""), std::string::npos);
}

TEST_F(TokenStoreTest, PersistenceRoundTrip)
{
    std::string tok;
    {
        TokenStore store(dir.string());
        tok = store.issueToken("alice").value();
    }
    // Reload from disk — should still recognise the token.
    TokenStore store2(dir.string());
    auto who = store2.lookupByToken(tok);
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
