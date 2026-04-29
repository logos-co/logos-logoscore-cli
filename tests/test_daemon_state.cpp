#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>
#include "daemon/daemon_state.h"
#include "config.h"

namespace fs = std::filesystem;

class DaemonStateTest : public ::testing::Test {
protected:
    std::string origHome;
    std::string origConfigDir;
    bool        origConfigDirSet = false;
    std::string testDir;

    void SetUp() override {
        testDir = (fs::temp_directory_path() / ("logoscore_test_state_" + std::to_string(getpid()))).string();
        fs::create_directories(testDir + "/.logoscore");

        // Cover all three layers Config::configDir() consults so the
        // tests can never escape into the user's real ~/.logoscore.
        // HOME alone isn't sufficient: an env-var override
        // (LOGOSCORE_CONFIG_DIR) or a process-wide setter from a
        // sibling test would shadow it. Save+restore each layer.
        const char* home = std::getenv("HOME");
        origHome = home ? home : "";
        setenv("HOME", testDir.c_str(), 1);

        const char* cd = std::getenv("LOGOSCORE_CONFIG_DIR");
        origConfigDirSet = cd != nullptr;
        origConfigDir = origConfigDirSet ? cd : "";
        unsetenv("LOGOSCORE_CONFIG_DIR");

        // Process-wide override (set by --config-dir in main.cpp). A
        // previous test in the same binary may have left one behind.
        Config::setConfigDir(QString());
    }

    void TearDown() override {
        setenv("HOME", origHome.c_str(), 1);
        if (origConfigDirSet)
            setenv("LOGOSCORE_CONFIG_DIR", origConfigDir.c_str(), 1);
        else
            unsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir(QString());
        std::error_code ec;
        fs::remove_all(testDir, ec);
    }
};

namespace {
DaemonState minimalState(const std::string& instanceId,
                         const std::vector<std::string>& dirs = {})
{
    DaemonState s;
    s.instanceId  = instanceId;
    s.pid         = getpid();
    s.startedAt   = currentUtcIso8601();
    s.modulesDirs = dirs;
    return s;
}
}

TEST_F(DaemonStateTest, Write_CreatesFile)
{
    EXPECT_TRUE(DaemonStateFile::write(minimalState("abc123", {"/path/to/modules"})));
    EXPECT_TRUE(fs::exists(DaemonStateFile::filePath()));
}

TEST_F(DaemonStateTest, WriteAndRead_RoundTripsBasic)
{
    DaemonState s = minimalState("inst123", {"/path/a", "/path/b"});
    s.loadModules     = "mod1,mod2";
    s.persistencePath = "/var/lib/logoscore";
    ASSERT_TRUE(DaemonStateFile::write(s));

    DaemonState got = DaemonStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.schemaVersion, kDaemonStateSchemaVersion);
    EXPECT_EQ(got.instanceId, "inst123");
    EXPECT_EQ(got.pid, getpid());
    EXPECT_EQ(got.modulesDirs.size(), 2u);
    EXPECT_EQ(got.modulesDirs[0], "/path/a");
    EXPECT_EQ(got.modulesDirs[1], "/path/b");
    EXPECT_EQ(got.loadModules, "mod1,mod2");
    EXPECT_EQ(got.persistencePath, "/var/lib/logoscore");
    EXPECT_FALSE(got.startedAt.empty());
}

TEST_F(DaemonStateTest, Read_InvalidWhenFileDoesNotExist)
{
    EXPECT_FALSE(DaemonStateFile::read().fileOk);
}

TEST_F(DaemonStateTest, Remove_DeletesFile)
{
    ASSERT_TRUE(DaemonStateFile::write(minimalState("inst3")));
    ASSERT_TRUE(fs::exists(DaemonStateFile::filePath()));
    EXPECT_TRUE(DaemonStateFile::remove());
    EXPECT_FALSE(fs::exists(DaemonStateFile::filePath()));
}

TEST_F(DaemonStateTest, Modules_RoundTripPerProtocol)
{
    DaemonState s = minimalState("instX", {"/mods"});
    std::vector<TransportInfo> coreTransports;
    coreTransports.push_back({"local", "", 0, "", true, "json"});
    coreTransports.push_back({"tcp",   "127.0.0.1", 6001, "", true, "json"});
    coreTransports.push_back({"tcp_ssl", "0.0.0.0", 6443, "/tmp/ca.pem", true, "cbor"});
    s.modules["core_service"] = std::move(coreTransports);

    ASSERT_TRUE(DaemonStateFile::write(s));

    DaemonState got = DaemonStateFile::read();
    ASSERT_EQ(got.modules.size(), 1u);
    auto coreIt = got.modules.find("core_service");
    ASSERT_NE(coreIt, got.modules.end());
    const auto& read = coreIt->second;
    ASSERT_EQ(read.size(), 3u);

    EXPECT_EQ(read[0].protocol, "local");
    EXPECT_EQ(read[0].codec, "json");

    EXPECT_EQ(read[1].protocol, "tcp");
    EXPECT_EQ(read[1].host, "127.0.0.1");
    EXPECT_EQ(read[1].port, 6001);
    EXPECT_EQ(read[1].codec, "json");

    EXPECT_EQ(read[2].protocol, "tcp_ssl");
    EXPECT_EQ(read[2].port, 6443);
    EXPECT_EQ(read[2].caFile, "/tmp/ca.pem");
    EXPECT_TRUE(read[2].verifyPeer);
    EXPECT_EQ(read[2].codec, "cbor");
}

TEST_F(DaemonStateTest, Modules_MultipleModulesRoundTrip)
{
    DaemonState s = minimalState("instY");
    s.modules["core_service"]      = {{"local"}, {"tcp", "127.0.0.1", 6001, "", true, "json"}};
    s.modules["capability_module"] = {{"local"}, {"tcp", "127.0.0.1", 6002, "", true, "json"}};
    ASSERT_TRUE(DaemonStateFile::write(s));

    DaemonState got = DaemonStateFile::read();
    ASSERT_EQ(got.modules.size(), 2u);
    EXPECT_EQ(got.modules.at("core_service").back().port, 6001);
    EXPECT_EQ(got.modules.at("capability_module").back().port, 6002);
}

TEST_F(DaemonStateTest, Modules_OmittedDefaultsToEmpty)
{
    ASSERT_TRUE(DaemonStateFile::write(minimalState("i")));
    DaemonState got = DaemonStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_TRUE(got.modules.empty());
}

TEST_F(DaemonStateTest, Read_RejectsUnknownSchemaVersion)
{
    fs::path p(DaemonStateFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p) << R"({"version":99,"instance_id":"x"})" << "\n";
    EXPECT_FALSE(DaemonStateFile::read().fileOk);
}

TEST_F(DaemonStateTest, Tokens_RoundTripWithExpiryAndLocalOnly)
{
    DaemonState s = minimalState("instTok");
    s.tokens.push_back({"auto",  "deadbeef", "2026-04-28T00:00:00Z", "", true});
    s.tokens.push_back({"alice", "abc123",   "2026-04-28T01:00:00Z", "2027-01-01T00:00:00Z", false});
    ASSERT_TRUE(DaemonStateFile::write(s));

    DaemonState got = DaemonStateFile::read();
    ASSERT_EQ(got.tokens.size(), 2u);
    EXPECT_EQ(got.tokens[0].name, "auto");
    EXPECT_EQ(got.tokens[0].hash, "deadbeef");
    EXPECT_TRUE(got.tokens[0].expiresAt.empty());
    EXPECT_TRUE(got.tokens[0].localOnly);
    EXPECT_EQ(got.tokens[1].name, "alice");
    EXPECT_EQ(got.tokens[1].expiresAt, "2027-01-01T00:00:00Z");
    EXPECT_FALSE(got.tokens[1].localOnly);
}

TEST_F(DaemonStateTest, Ssl_RoundTripsPaths)
{
    DaemonState s = minimalState("instSsl");
    s.sslCert = "/etc/ssl/cert.pem";
    s.sslKey  = "/etc/ssl/key.pem";
    s.sslCa   = "/etc/ssl/ca.pem";
    ASSERT_TRUE(DaemonStateFile::write(s));

    DaemonState got = DaemonStateFile::read();
    EXPECT_EQ(got.sslCert, "/etc/ssl/cert.pem");
    EXPECT_EQ(got.sslKey,  "/etc/ssl/key.pem");
    EXPECT_EQ(got.sslCa,   "/etc/ssl/ca.pem");
}
