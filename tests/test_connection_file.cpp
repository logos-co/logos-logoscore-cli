#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include "daemon/connection_file.h"
#include "config.h"

namespace fs = std::filesystem;

class ConnectionFileTest : public ::testing::Test {
protected:
    std::string origHome;
    std::string testDir;

    void SetUp() override {
        testDir = (fs::temp_directory_path() / ("logoscore_test_conn_" + std::to_string(getpid()))).string();
        fs::create_directories(testDir + "/.logoscore");
        const char* home = std::getenv("HOME");
        origHome = home ? home : "";
        setenv("HOME", testDir.c_str(), 1);
    }

    void TearDown() override {
        setenv("HOME", origHome.c_str(), 1);
        std::error_code ec;
        fs::remove_all(testDir, ec);
    }
};

TEST_F(ConnectionFileTest, Write_CreatesFile)
{
    bool ok = ConnectionFile::write("abc123", "token-uuid", 12345,
                                    {"/path/to/modules"});
    EXPECT_TRUE(ok);
    EXPECT_TRUE(fs::exists(ConnectionFile::filePath()));
}

TEST_F(ConnectionFileTest, WriteAndRead_RoundTrips)
{
    std::vector<std::string> dirs = {"/path/a", "/path/b"};
    bool ok = ConnectionFile::write("inst123", "tok456", 99999, dirs);
    ASSERT_TRUE(ok);

    ConnectionInfo info = ConnectionFile::read();
    EXPECT_EQ(info.instanceId, "inst123");
    EXPECT_EQ(info.token, "tok456");
    EXPECT_EQ(info.pid, 99999);
    EXPECT_EQ(info.modulesDirs.size(), 2u);
    EXPECT_EQ(info.modulesDirs[0], "/path/a");
    EXPECT_EQ(info.modulesDirs[1], "/path/b");
    EXPECT_FALSE(info.startedAt.empty());
}

TEST_F(ConnectionFileTest, Read_InvalidWhenFileDoesNotExist)
{
    ConnectionInfo info = ConnectionFile::read();
    EXPECT_FALSE(info.valid);
}

TEST_F(ConnectionFileTest, Read_ValidWhenPidIsAlive)
{
    // Use our own PID which is definitely alive
    int64_t ourPid = getpid();
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
    ASSERT_TRUE(fs::exists(ConnectionFile::filePath()));

    bool ok = ConnectionFile::remove();
    EXPECT_TRUE(ok);
    EXPECT_FALSE(fs::exists(ConnectionFile::filePath()));
}

TEST_F(ConnectionFileTest, IsStale_TrueWhenPidDead)
{
    ConnectionFile::write("inst4", "tok4", 99999999, {});
    EXPECT_TRUE(ConnectionFile::isStale());
}

TEST_F(ConnectionFileTest, IsStale_FalseWhenPidAlive)
{
    int64_t ourPid = getpid();
    ConnectionFile::write("inst5", "tok5", ourPid, {});
    EXPECT_FALSE(ConnectionFile::isStale());
}

TEST_F(ConnectionFileTest, IsStale_TrueWhenNoFile)
{
    EXPECT_TRUE(ConnectionFile::isStale());
}
