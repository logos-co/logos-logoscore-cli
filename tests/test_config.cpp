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
        // These tests describe the logosctl surface. The flavor defaults to
        // Legacy so that anything which forgets to set it behaves like the
        // tool that exists today, so say which one we mean.
        Config::setFlavor(Config::Flavor::Modern);
        testDir = getTempDir() + "/logosctl_test_config_" + std::to_string(getpid());
        std::filesystem::create_directories(testDir + "/.logosctl");

        const char* h = std::getenv("HOME");
        origHome = h ? std::string(h) : "";
        setenv("HOME", testDir.c_str(), 1);

        unsetenv("LOGOSCTL_TOKEN");
        unsetenv("LOGOSCTL_CONFIG_DIR");
        Config::setConfigDir("");
    }

    void TearDown() override {
        setenv("HOME", origHome.c_str(), 1);
        unsetenv("LOGOSCTL_CONFIG_DIR");
        Config::setConfigDir("");
        std::filesystem::remove_all(testDir);
    }
};

TEST_F(ConfigTest, ConfigDir_ReturnsHomeLogosctl)
{
    std::string dir = Config::configDir();
    const std::string suffix = "/.logosctl";
    EXPECT_TRUE(dir.size() > suffix.size() &&
                dir.substr(dir.size() - suffix.size()) == suffix);
}

// ----------------------------------------------------------------------
// Token resolution
// ----------------------------------------------------------------------

TEST_F(ConfigTest, GetToken_EnvVarReturned)
{
    setenv("LOGOSCTL_TOKEN", "env-token", 1);
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
    EXPECT_EQ(Config::daemonConfigPath(), cfg + "/daemon/config.yaml");
    EXPECT_EQ(Config::daemonStatePath(),  cfg + "/daemon/state.json");
    EXPECT_EQ(Config::daemonTokensPath(), cfg + "/daemon/tokens.json");
    EXPECT_EQ(Config::daemonTokensDir(),  cfg + "/daemon/tokens");
    EXPECT_EQ(Config::clientDir(),        cfg + "/client");
    EXPECT_EQ(Config::clientConfigPath(), cfg + "/client/config.yaml");
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
    setenv("LOGOSCTL_CONFIG_DIR", alt.c_str(), 1);

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

    setenv("LOGOSCTL_CONFIG_DIR", envDir.c_str(), 1);
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

    setenv("LOGOSCTL_CONFIG_DIR", envDir.c_str(), 1);
    Config::setConfigDir(setterDir);
    ASSERT_EQ(Config::configDir(), setterDir);

    Config::setConfigDir("");
    EXPECT_EQ(Config::configDir(), envDir);
}

// The two binaries must not share a byte of state: a bad logosctl session
// cannot be allowed to disturb a working logoscore deployment. That isolation
// is the whole reason `logoscore` can keep shipping unchanged while logosctl
// is validated, so pin it down.
TEST_F(ConfigTest, LegacyAndModernFlavorsShareNothing)
{
    Config::setConfigDir("");   // fall back to the per-flavor default

    Config::setFlavor(Config::Flavor::Legacy);
    const std::string legacyDir  = Config::configDir();
    const std::string legacyCfg  = Config::daemonConfigPath();
    const std::string legacyClnt = Config::clientConfigPath();

    Config::setFlavor(Config::Flavor::Modern);
    const std::string modernDir  = Config::configDir();
    const std::string modernCfg  = Config::daemonConfigPath();
    const std::string modernClnt = Config::clientConfigPath();

    EXPECT_NE(legacyDir, modernDir);
    EXPECT_NE(std::string::npos, legacyDir.find(".logoscore"));
    EXPECT_NE(std::string::npos, modernDir.find(".logosctl"));

    // logoscore keeps writing JSON so an existing deployment's config stays
    // readable by the tool that wrote it; logosctl writes YAML.
    EXPECT_NE(std::string::npos, legacyCfg.find("config.json"));
    EXPECT_NE(std::string::npos, modernCfg.find("config.yaml"));
    EXPECT_NE(std::string::npos, legacyClnt.find("config.json"));
    EXPECT_NE(std::string::npos, modernClnt.find("config.yaml"));
}

// The session is portable by default -- every subdirectory lives inside the
// config dir -- but each one can be redirected, so a shared keyring or a cache
// on a bigger disk doesn't force you to give that up wholesale.
TEST_F(ConfigTest, SessionDirsDefaultInsideTheSession)
{
    const std::string cfg = testDir;
    Config::setConfigDir(cfg);
    for (auto which : {Config::SessionDir::Modules, Config::SessionDir::Plugins,
                       Config::SessionDir::Keyring, Config::SessionDir::Data,
                       Config::SessionDir::Cache})
        Config::setSessionDirOverride(which, "");

    EXPECT_EQ(Config::modulesDir(), cfg + "/modules");
    EXPECT_EQ(Config::pluginsDir(), cfg + "/plugins");
    EXPECT_EQ(Config::keyringDir(), cfg + "/keyring");
    EXPECT_EQ(Config::dataDir(),    cfg + "/data");
    EXPECT_EQ(Config::cacheDir(),   cfg + "/cache");
}

TEST_F(ConfigTest, SessionDirOverrideResolvesByForm)
{
    const std::string cfg = testDir;
    Config::setConfigDir(cfg);

    // Absolute: deliberately outside the session.
    Config::setSessionDirOverride(Config::SessionDir::Keyring, "/opt/shared/keys");
    EXPECT_EQ(Config::keyringDir(), "/opt/shared/keys");

    // Relative: still inside the session, so it stays portable.
    Config::setSessionDirOverride(Config::SessionDir::Modules, "my-modules");
    EXPECT_EQ(Config::modulesDir(), cfg + "/my-modules");

    // `~` is natural to write in a config file and would otherwise be taken
    // as a relative path, producing <configDir>/~/... which exists nowhere.
    if (const char* home = std::getenv("HOME")) {
        Config::setSessionDirOverride(Config::SessionDir::Cache, "~/lgx-cache");
        EXPECT_EQ(Config::cacheDir(), std::string(home) + "/lgx-cache");
    }

    // Clearing restores the default.
    Config::setSessionDirOverride(Config::SessionDir::Keyring, "");
    EXPECT_EQ(Config::keyringDir(), cfg + "/keyring");

    for (auto which : {Config::SessionDir::Modules, Config::SessionDir::Cache})
        Config::setSessionDirOverride(which, "");
}

// An override is resolved once, when it is set. Relocating the session
// afterwards must not silently drag an absolute override along with it.
TEST_F(ConfigTest, SessionDirOverrideIsResolvedAtSetTime)
{
    Config::setConfigDir(testDir);
    Config::setSessionDirOverride(Config::SessionDir::Modules, "my-modules");
    const std::string before = Config::modulesDir();

    Config::setConfigDir(testDir + "-moved");
    EXPECT_EQ(Config::modulesDir(), before);

    Config::setSessionDirOverride(Config::SessionDir::Modules, "");
}
