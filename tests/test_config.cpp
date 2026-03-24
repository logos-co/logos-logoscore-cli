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

        // Clear LOGOSCORE_TOKEN
        qunsetenv("LOGOSCORE_TOKEN");
    }

    void TearDown() override {
        // Restore HOME
        qputenv("HOME", origHome.toUtf8());

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
