#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "config.h"

static std::string getTempDir()
{
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    return std::string(tmp);
}

class ConfigTest : public ::testing::Test {
protected:
    std::string origHome;
    std::string testDir;

    void SetUp() override {
        testDir = getTempDir() + "/logoscore_test_config_" + std::to_string(getpid());
        std::filesystem::create_directories(testDir + "/.logoscore");

        const char* h = std::getenv("HOME");
        origHome = h ? std::string(h) : "";
        setenv("HOME", testDir.c_str(), 1);

        unsetenv("LOGOSCORE_TOKEN");
        unsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir("");
    }

    void TearDown() override {
        setenv("HOME", origHome.c_str(), 1);
        unsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir("");
        std::filesystem::remove_all(testDir);
    }
};

TEST_F(ConfigTest, ConfigDir_ReturnsHomeLogoscore)
{
    std::string dir = Config::configDir();
    const std::string suffix = "/.logoscore";
    EXPECT_TRUE(dir.size() > suffix.size() &&
                dir.substr(dir.size() - suffix.size()) == suffix);
}

// ----------------------------------------------------------------------
// Token resolution
// ----------------------------------------------------------------------

TEST_F(ConfigTest, GetToken_EnvVarReturned)
{
    setenv("LOGOSCORE_TOKEN", "env-token", 1);
    EXPECT_EQ(Config::getToken(), "env-token");
}

TEST_F(ConfigTest, GetToken_EmptyWhenNoEnv)
{
    EXPECT_TRUE(Config::getToken().empty());
}

// ----------------------------------------------------------------------
// Path helpers — verify the daemon/ and client/ subdir layout is honored.
// ----------------------------------------------------------------------

TEST_F(ConfigTest, Paths_DaemonAndClientUnderConfigDir)
{
    const std::string cfg = Config::configDir();
    EXPECT_EQ(Config::daemonDir(),        cfg + "/daemon");
    EXPECT_EQ(Config::daemonConfigPath(), cfg + "/daemon/config.json");
    EXPECT_EQ(Config::daemonStatePath(),  cfg + "/daemon/state.json");
    EXPECT_EQ(Config::daemonTokensPath(), cfg + "/daemon/tokens.json");
    EXPECT_EQ(Config::daemonTokensDir(),  cfg + "/daemon/tokens");
    EXPECT_EQ(Config::clientDir(),        cfg + "/client");
    EXPECT_EQ(Config::clientConfigPath(), cfg + "/client/config.json");
    EXPECT_EQ(Config::clientTokenPath("auto.json"), cfg + "/client/auto.json");
}

// ----------------------------------------------------------------------
// BUG-019: clientTokenPath() must not let a token_file value escape the
// client/ dir. The filename comes from --token-file, the
// LOGOSCORE_CLIENT_TOKEN_FILE env var, or client/config.json's token_file
// field; a value like "../daemon/tokens.json" or "../../etc/passwd" would
// otherwise resolve outside client/ and be read as a credential file.
// ----------------------------------------------------------------------

TEST_F(ConfigTest, ClientTokenPath_AcceptsSimpleFilenames)
{
    const std::string cfg = Config::configDir();
    EXPECT_EQ(Config::clientTokenPath("auto.json"),  cfg + "/client/auto.json");
    EXPECT_EQ(Config::clientTokenPath("alice.json"), cfg + "/client/alice.json");
}

TEST_F(ConfigTest, ClientTokenPath_RejectsTraversal)
{
    const std::string clientDir = Config::clientDir();
    // Every traversal/absolute attempt must stay strictly inside client/:
    // the resolved path must begin with "<clientDir>/" and must not contain
    // a ".." component that climbs out.
    for (const std::string bad : {
            std::string("../daemon/tokens.json"),
            std::string("../../etc/passwd"),
            std::string("sub/dir/token.json"),
            std::string("/etc/passwd"),
        }) {
        const std::string got = Config::clientTokenPath(bad);
        EXPECT_EQ(got.rfind(clientDir + "/", 0), 0u)
            << "clientTokenPath('" << bad << "') escaped client/: " << got;
        EXPECT_EQ(got.find(".."), std::string::npos)
            << "clientTokenPath('" << bad << "') still contains '..': " << got;
    }
}

// ----------------------------------------------------------------------
// Config-dir override precedence
// ----------------------------------------------------------------------

TEST_F(ConfigTest, ConfigDir_EnvVarOverridesHome)
{
    const std::string alt = testDir + "/alt-config";
    std::filesystem::create_directories(alt);
    setenv("LOGOSCORE_CONFIG_DIR", alt.c_str(), 1);

    EXPECT_EQ(Config::configDir(), alt);
    EXPECT_EQ(Config::daemonConfigPath().substr(0, alt.size()), alt);
    EXPECT_EQ(Config::clientConfigPath().substr(0, alt.size()), alt);
}

TEST_F(ConfigTest, ConfigDir_SetterOverridesEnvVar)
{
    const std::string envDir    = testDir + "/env-config";
    const std::string setterDir = testDir + "/setter-config";
    std::filesystem::create_directories(envDir);
    std::filesystem::create_directories(setterDir);

    setenv("LOGOSCORE_CONFIG_DIR", envDir.c_str(), 1);
    Config::setConfigDir(setterDir);

    EXPECT_EQ(Config::configDir(), setterDir)
        << "explicit setter (from --config-dir) must win over env var";
}

TEST_F(ConfigTest, ConfigDir_ClearingSetterFallsBackToEnv)
{
    const std::string envDir    = testDir + "/env-config";
    const std::string setterDir = testDir + "/setter-config";
    std::filesystem::create_directories(envDir);
    std::filesystem::create_directories(setterDir);

    setenv("LOGOSCORE_CONFIG_DIR", envDir.c_str(), 1);
    Config::setConfigDir(setterDir);
    ASSERT_EQ(Config::configDir(), setterDir);

    Config::setConfigDir("");
    EXPECT_EQ(Config::configDir(), envDir);
}
