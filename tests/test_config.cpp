#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdlib>
#include "config.h"

class ConfigTest : public ::testing::Test {
protected:
    QString origHome;
    QString testDir;

    void SetUp() override {
        // Create a temp directory to use as fake home
        testDir = QDir::tempPath() + "/logoscore_test_config_" + QString::number(QCoreApplication::applicationPid());
        QDir().mkpath(testDir + "/.logoscore");

        // Save and override HOME
        origHome = qEnvironmentVariable("HOME");
        qputenv("HOME", testDir.toUtf8());

        // Clear token + config-dir overrides so each test starts from a known
        // state (setConfigDir and LOGOSCORE_CONFIG_DIR both bypass HOME).
        qunsetenv("LOGOSCORE_TOKEN");
        qunsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir(QString());
    }

    void TearDown() override {
        // Restore HOME
        qputenv("HOME", origHome.toUtf8());

        // Clear overrides so they don't leak between tests.
        qunsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir(QString());

        // Clean up temp dir
        QDir(testDir).removeRecursively();
    }

    void writeJsonFile(const QString& path, const QJsonObject& obj) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(obj).toJson());
        file.close();
    }
};

TEST_F(ConfigTest, ConfigDir_ReturnsHomeLogoscore)
{
    QString dir = Config::configDir();
    EXPECT_TRUE(dir.endsWith("/.logoscore"));
}

TEST_F(ConfigTest, GetToken_EnvVarHasHighestPriority)
{
    // Write token to config file
    writeJsonFile(testDir + "/.logoscore/config.json",
                  QJsonObject{{"token", "config-token"}});

    // Write token to connection file
    writeJsonFile(testDir + "/.logoscore/daemon.json",
                  QJsonObject{{"token", "daemon-token"}});

    // Set env var
    qputenv("LOGOSCORE_TOKEN", "env-token");

    EXPECT_EQ(Config::getToken(), "env-token");
}

TEST_F(ConfigTest, GetToken_ConfigFileSecondPriority)
{
    writeJsonFile(testDir + "/.logoscore/config.json",
                  QJsonObject{{"token", "config-token"}});
    writeJsonFile(testDir + "/.logoscore/daemon.json",
                  QJsonObject{{"token", "daemon-token"}});

    EXPECT_EQ(Config::getToken(), "config-token");
}

TEST_F(ConfigTest, GetToken_ConnectionFileThirdPriority)
{
    writeJsonFile(testDir + "/.logoscore/daemon.json",
                  QJsonObject{{"token", "daemon-token"}});

    EXPECT_EQ(Config::getToken(), "daemon-token");
}

TEST_F(ConfigTest, GetToken_ReturnsEmptyWhenNoTokenAvailable)
{
    EXPECT_TRUE(Config::getToken().isEmpty());
}

TEST_F(ConfigTest, Load_ReturnsConfigObject)
{
    QJsonObject config;
    config["token"] = "test-token";
    config["custom_setting"] = "value";
    writeJsonFile(testDir + "/.logoscore/config.json", config);

    QJsonObject loaded = Config::load();
    EXPECT_EQ(loaded.value("token").toString(), "test-token");
    EXPECT_EQ(loaded.value("custom_setting").toString(), "value");
}

TEST_F(ConfigTest, Load_ReturnsEmptyOnMissingFile)
{
    QJsonObject loaded = Config::load();
    EXPECT_TRUE(loaded.isEmpty());
}

// --------------------------------------------------------------------------
// Config-dir override: explicit setter > LOGOSCORE_CONFIG_DIR env > ~/.logoscore.
// These guarantee --config-dir from main() takes effect everywhere and that
// parallel logoscore instances with distinct --config-dir values land in
// distinct trees.
// --------------------------------------------------------------------------

TEST_F(ConfigTest, ConfigDir_EnvVarOverridesHome)
{
    const QString alt = testDir + "/alt-config";
    QDir().mkpath(alt);
    qputenv("LOGOSCORE_CONFIG_DIR", alt.toUtf8());

    EXPECT_EQ(Config::configDir(), alt);
    EXPECT_TRUE(Config::configFilePath().startsWith(alt));
    EXPECT_TRUE(Config::connectionFilePath().startsWith(alt));
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

TEST_F(ConfigTest, ConfigDir_OverrideRoutesTokenLookupToOverriddenDir)
{
    const QString alt = testDir + "/alt-config";
    QDir().mkpath(alt);
    Config::setConfigDir(alt);

    // Write a token under the OVERRIDDEN dir; the legacy ~/.logoscore/config.json
    // also exists with a different token. The override must win.
    writeJsonFile(alt + "/config.json",
                  QJsonObject{{"token", "override-token"}});
    writeJsonFile(testDir + "/.logoscore/config.json",
                  QJsonObject{{"token", "home-token"}});

    EXPECT_EQ(Config::getToken(), "override-token");
}
