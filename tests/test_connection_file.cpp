#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include "daemon/connection_file.h"
#include "config.h"

class ConnectionFileTest : public ::testing::Test {
protected:
    QString origHome;
    QString testDir;

    void SetUp() override {
        testDir = QDir::tempPath() + "/logoscore_test_conn_" + QString::number(QCoreApplication::applicationPid());
        QDir().mkpath(testDir + "/.logoscore");
        origHome = qEnvironmentVariable("HOME");
        qputenv("HOME", testDir.toUtf8());
    }

    void TearDown() override {
        qputenv("HOME", origHome.toUtf8());
        QDir(testDir).removeRecursively();
    }
};

TEST_F(ConnectionFileTest, Write_CreatesFile)
{
    bool ok = ConnectionFile::write("abc123", "token-uuid", 12345,
                                    {"/path/to/modules"});
    EXPECT_TRUE(ok);
    EXPECT_TRUE(QFile::exists(ConnectionFile::filePath()));
}

TEST_F(ConnectionFileTest, WriteAndRead_RoundTrips)
{
    QStringList dirs = {"/path/a", "/path/b"};
    bool ok = ConnectionFile::write("inst123", "tok456", 99999, dirs);
    ASSERT_TRUE(ok);

    ConnectionInfo info = ConnectionFile::read();
    EXPECT_EQ(info.instanceId, "inst123");
    EXPECT_EQ(info.token, "tok456");
    EXPECT_EQ(info.pid, 99999);
    EXPECT_EQ(info.modulesDirs.size(), 2);
    EXPECT_EQ(info.modulesDirs[0], "/path/a");
    EXPECT_EQ(info.modulesDirs[1], "/path/b");
    EXPECT_TRUE(info.startedAt.isValid());
}

TEST_F(ConnectionFileTest, Read_InvalidWhenFileDoesNotExist)
{
    ConnectionInfo info = ConnectionFile::read();
    EXPECT_FALSE(info.valid);
}

TEST_F(ConnectionFileTest, Read_ValidWhenPidIsAlive)
{
    // Use our own PID which is definitely alive
    qint64 ourPid = QCoreApplication::applicationPid();
    ConnectionFile::write("inst1", "tok1", ourPid, {});

    ConnectionInfo info = ConnectionFile::read();
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.pid, ourPid);
}

TEST_F(ConnectionFileTest, Read_InvalidWhenPidIsDead)
{
    // Use a PID that's very unlikely to be alive
    // PID 99999999 shouldn't exist
    ConnectionFile::write("inst2", "tok2", 99999999, {});

    ConnectionInfo info = ConnectionFile::read();
    EXPECT_FALSE(info.valid);
}

TEST_F(ConnectionFileTest, Remove_DeletesFile)
{
    ConnectionFile::write("inst3", "tok3", 12345, {});
    ASSERT_TRUE(QFile::exists(ConnectionFile::filePath()));

    bool ok = ConnectionFile::remove();
    EXPECT_TRUE(ok);
    EXPECT_FALSE(QFile::exists(ConnectionFile::filePath()));
}

TEST_F(ConnectionFileTest, IsStale_TrueWhenPidDead)
{
    ConnectionFile::write("inst4", "tok4", 99999999, {});
    EXPECT_TRUE(ConnectionFile::isStale());
}

TEST_F(ConnectionFileTest, IsStale_FalseWhenPidAlive)
{
    qint64 ourPid = QCoreApplication::applicationPid();
    ConnectionFile::write("inst5", "tok5", ourPid, {});
    EXPECT_FALSE(ConnectionFile::isStale());
}

TEST_F(ConnectionFileTest, IsStale_TrueWhenNoFile)
{
    EXPECT_TRUE(ConnectionFile::isStale());
}
