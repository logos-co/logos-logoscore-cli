#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <cstdlib>
#include "config.h"

class ConfigTest : public ::testing::Test {
protected:
    QString origHome;
    QString testDir;

    void SetUp() override {
        testDir = QDir::tempPath() + "/logoscore_test_config_" + QString::number(QCoreApplication::applicationPid());
        QDir().mkpath(testDir + "/.logoscore");

        origHome = qEnvironmentVariable("HOME");
        qputenv("HOME", testDir.toUtf8());

        qunsetenv("LOGOSCORE_TOKEN");
        qunsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir(QString());
    }

    void TearDown() override {
        qputenv("HOME", origHome.toUtf8());
        qunsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir(QString());
        QDir(testDir).removeRecursively();
    }
};

TEST_F(ConfigTest, ConfigDir_ReturnsHomeLogoscore)
{
    QString dir = Config::configDir();
    EXPECT_TRUE(dir.endsWith("/.logoscore"));
}

// ----------------------------------------------------------------------
// Token resolution
// Post-config-split, Config::getToken() only knows about the env override.
// The fallback to client/<token_file> lives in ClientState (a layer up).
// ----------------------------------------------------------------------

TEST_F(ConfigTest, GetToken_EnvVarReturned)
{
    qputenv("LOGOSCORE_TOKEN", "env-token");
    EXPECT_EQ(Config::getToken(), "env-token");
}

TEST_F(ConfigTest, GetToken_EmptyWhenNoEnv)
{
    EXPECT_TRUE(Config::getToken().isEmpty());
}

// ----------------------------------------------------------------------
// Path helpers — verify the daemon/ and client/ subdir layout is honored.
// ----------------------------------------------------------------------

TEST_F(ConfigTest, Paths_DaemonAndClientUnderConfigDir)
{
    const QString cfg = Config::configDir();
    EXPECT_EQ(Config::daemonDir(),         cfg + "/daemon");
    EXPECT_EQ(Config::daemonConfigPath(),  cfg + "/daemon/config.json");
    EXPECT_EQ(Config::daemonStatePath(),   cfg + "/daemon/state.json");
    EXPECT_EQ(Config::daemonTokensPath(),  cfg + "/daemon/tokens.json");
    EXPECT_EQ(Config::daemonTokensDir(),   cfg + "/daemon/tokens");
    EXPECT_EQ(Config::clientDir(),         cfg + "/client");
    EXPECT_EQ(Config::clientConfigPath(),  cfg + "/client/config.json");
    EXPECT_EQ(Config::clientTokenPath("auto.json"),
              cfg + "/client/auto.json");
}

// ----------------------------------------------------------------------
// Config-dir override precedence
// ----------------------------------------------------------------------

TEST_F(ConfigTest, ConfigDir_EnvVarOverridesHome)
{
    const QString alt = testDir + "/alt-config";
    QDir().mkpath(alt);
    qputenv("LOGOSCORE_CONFIG_DIR", alt.toUtf8());

    EXPECT_EQ(Config::configDir(), alt);
    EXPECT_TRUE(Config::daemonConfigPath().startsWith(alt));
    EXPECT_TRUE(Config::clientConfigPath().startsWith(alt));
}

TEST_F(ConfigTest, ConfigDir_SetterOverridesEnvVar)
{
    const QString envDir = testDir + "/env-config";
    const QString setterDir = testDir + "/setter-config";
    QDir().mkpath(envDir);
    QDir().mkpath(setterDir);

    qputenv("LOGOSCORE_CONFIG_DIR", envDir.toUtf8());
    Config::setConfigDir(setterDir);

    EXPECT_EQ(Config::configDir(), setterDir)
        << "explicit setter (from --config-dir) must win over env var";
}

TEST_F(ConfigTest, ConfigDir_ClearingSetterFallsBackToEnv)
{
    const QString envDir = testDir + "/env-config";
    const QString setterDir = testDir + "/setter-config";
    QDir().mkpath(envDir);
    QDir().mkpath(setterDir);

    qputenv("LOGOSCORE_CONFIG_DIR", envDir.toUtf8());
    Config::setConfigDir(setterDir);
    ASSERT_EQ(Config::configDir(), setterDir);

    Config::setConfigDir(QString());
    EXPECT_EQ(Config::configDir(), envDir);
}
