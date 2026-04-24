#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
    EXPECT_FALSE(info.fileOk);
}

TEST_F(ConnectionFileTest, Read_FileOkIndependentOfPid)
{
    // File parses cleanly even if the pid is bogus — fileOk is purely
    // about "is the on-disk state parseable?", not liveness.
    ConnectionFile::write("inst2", "tok2", 99999999, {});
    ConnectionInfo info = ConnectionFile::read();
    EXPECT_TRUE(info.fileOk);
    EXPECT_EQ(info.pid, 99999999);
}

TEST_F(ConnectionFileTest, Remove_DeletesFile)
{
    ConnectionFile::write("inst3", "tok3", 12345, {});
    ASSERT_TRUE(fs::exists(ConnectionFile::filePath()));

    bool ok = ConnectionFile::remove();
    EXPECT_TRUE(ok);
    EXPECT_FALSE(fs::exists(ConnectionFile::filePath()));
}

TEST_F(ConnectionFileTest, Transports_RoundTripPerProtocol)
{
    // One entry per distinct shape: local (no host/port/codec),
    // tcp (host/port + codec), tcp_ssl (adds ca + verify_peer + codec).
    std::vector<TransportInfo> ts;
    ts.push_back({"local", "", 0, "", true, "json"});
    ts.push_back({"tcp",   "127.0.0.1", 6001, "", true, "json"});
    ts.push_back({"tcp_ssl", "0.0.0.0", 6443, "/tmp/ca.pem", true, "cbor"});

    ASSERT_TRUE(ConnectionFile::write(
        "instX", "tokX", static_cast<int64_t>(getpid()), {"/mods"}, ts));

    ConnectionInfo info = ConnectionFile::read();
    ASSERT_EQ(info.transports.size(), 3u);

    EXPECT_EQ(info.transports[0].protocol, "local");
    // Codec is meaningless for local but defaults to "json" on read.
    EXPECT_EQ(info.transports[0].codec, "json");

    EXPECT_EQ(info.transports[1].protocol, "tcp");
    EXPECT_EQ(info.transports[1].host, "127.0.0.1");
    EXPECT_EQ(info.transports[1].port, 6001);
    EXPECT_EQ(info.transports[1].codec, "json");

    EXPECT_EQ(info.transports[2].protocol, "tcp_ssl");
    EXPECT_EQ(info.transports[2].port, 6443);
    EXPECT_EQ(info.transports[2].caFile, "/tmp/ca.pem");
    EXPECT_TRUE(info.transports[2].verifyPeer);
    EXPECT_EQ(info.transports[2].codec, "cbor");
}

TEST_F(ConnectionFileTest, Transports_OmittedDefaultsToEmpty)
{
    // Writer without a transports vector: old callers still work and read
    // back an empty transports list (not an error).
    ASSERT_TRUE(ConnectionFile::write("i", "t",
                                      static_cast<int64_t>(getpid()), {}));
    ConnectionInfo info = ConnectionFile::read();
    EXPECT_TRUE(info.transports.empty());
}

TEST_F(ConnectionFileTest, Transports_CodecDefaultsToJsonForTcp)
{
    // Older daemons that wrote transports without a codec field should
    // read back as "json" (so newer clients don't break on older daemons).
    std::vector<TransportInfo> ts;
    TransportInfo t;
    t.protocol = "tcp"; t.host = "127.0.0.1"; t.port = 6000;
    t.codec = "";  // simulate missing codec
    ts.push_back(t);
    ASSERT_TRUE(ConnectionFile::write("i", "t",
                                      static_cast<int64_t>(getpid()), {}, ts));
    ConnectionInfo info = ConnectionFile::read();
    ASSERT_EQ(info.transports.size(), 1u);
    EXPECT_EQ(info.transports[0].codec, "json");
}
