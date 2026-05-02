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
        const char* home = std::getenv("HOME");
        origHome = home ? home : "";
        setenv("HOME", testDir.c_str(), 1);

        const char* cd = std::getenv("LOGOSCORE_CONFIG_DIR");
        origConfigDirSet = cd != nullptr;
        origConfigDir = origConfigDirSet ? cd : "";
        unsetenv("LOGOSCORE_CONFIG_DIR");

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

DaemonRuntimeState minimalState(const std::string& instanceId,
                                const std::vector<std::string>& dirs = {})
{
    DaemonRuntimeState s;
    s.instanceId  = instanceId;
    s.pid         = getpid();
    s.startedAt   = currentUtcIso8601();
    s.resolved.modulesDirs = dirs;
    return s;
}

DaemonConfig sampleConfig()
{
    DaemonConfig cfg;
    cfg.modulesDirs     = {"/path/a", "/path/b"};
    cfg.loadModules     = "mod1,mod2";
    cfg.persistencePath = "/var/lib/logoscore";
    cfg.modules["core_service"]      = {{"local"}, {"tcp", "127.0.0.1", 6001, "", true, "json"}};
    cfg.modules["capability_module"] = {{"local"}};
    cfg.sslCert = "/etc/ssl/cert.pem";
    cfg.sslKey  = "/etc/ssl/key.pem";
    cfg.sslCa   = "/etc/ssl/ca.pem";
    cfg.insecureTcp = true;
    return cfg;
}

} // namespace

// -- DaemonRuntimeStateFile (state.json) ----------------------------------

TEST_F(DaemonStateTest, RuntimeState_WriteCreatesFile)
{
    EXPECT_TRUE(DaemonRuntimeStateFile::write(minimalState("abc123", {"/path/to/modules"})));
    EXPECT_TRUE(fs::exists(DaemonRuntimeStateFile::filePath()));
}

TEST_F(DaemonStateTest, RuntimeState_RoundTripsResolvedFields)
{
    DaemonRuntimeState s = minimalState("inst123", {"/path/a", "/path/b"});
    s.configSource = "cli";
    s.resolved.loadModules     = "mod1,mod2";
    s.resolved.persistencePath = "/var/lib/logoscore";
    ASSERT_TRUE(DaemonRuntimeStateFile::write(s));

    DaemonRuntimeState got = DaemonRuntimeStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.schemaVersion, kDaemonRuntimeStateSchemaVersion);
    EXPECT_EQ(got.instanceId, "inst123");
    EXPECT_EQ(got.pid, getpid());
    EXPECT_EQ(got.configSource, "cli");
    EXPECT_EQ(got.resolved.modulesDirs.size(), 2u);
    EXPECT_EQ(got.resolved.modulesDirs[0], "/path/a");
    EXPECT_EQ(got.resolved.loadModules, "mod1,mod2");
    EXPECT_EQ(got.resolved.persistencePath, "/var/lib/logoscore");
    EXPECT_FALSE(got.startedAt.empty());
}

TEST_F(DaemonStateTest, RuntimeState_InvalidWhenFileDoesNotExist)
{
    EXPECT_FALSE(DaemonRuntimeStateFile::read().fileOk);
}

TEST_F(DaemonStateTest, RuntimeState_RemoveDeletesFile)
{
    ASSERT_TRUE(DaemonRuntimeStateFile::write(minimalState("inst3")));
    ASSERT_TRUE(fs::exists(DaemonRuntimeStateFile::filePath()));
    EXPECT_TRUE(DaemonRuntimeStateFile::remove());
    EXPECT_FALSE(fs::exists(DaemonRuntimeStateFile::filePath()));
}

TEST_F(DaemonStateTest, RuntimeState_RejectsUnknownSchemaVersion)
{
    fs::path p(DaemonRuntimeStateFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p) << R"({"version":99,"instance_id":"x"})" << "\n";
    EXPECT_FALSE(DaemonRuntimeStateFile::read().fileOk);
}

TEST_F(DaemonStateTest, RuntimeState_ResolvedModulesRoundTripPerProtocol)
{
    DaemonRuntimeState s = minimalState("instX", {"/mods"});
    std::vector<TransportInfo> coreTransports;
    coreTransports.push_back({"local", "", 0, "", true, "json"});
    coreTransports.push_back({"tcp",   "127.0.0.1", 6001, "", true, "json"});
    coreTransports.push_back({"tcp_ssl", "0.0.0.0", 6443, "/tmp/ca.pem", true, "cbor"});
    s.resolved.modules["core_service"] = std::move(coreTransports);
    ASSERT_TRUE(DaemonRuntimeStateFile::write(s));

    DaemonRuntimeState got = DaemonRuntimeStateFile::read();
    ASSERT_EQ(got.resolved.modules.size(), 1u);
    const auto& read = got.resolved.modules.at("core_service");
    ASSERT_EQ(read.size(), 3u);
    EXPECT_EQ(read[0].protocol, "local");
    EXPECT_EQ(read[1].protocol, "tcp");
    EXPECT_EQ(read[1].port, 6001);
    EXPECT_EQ(read[2].protocol, "tcp_ssl");
    EXPECT_EQ(read[2].caFile, "/tmp/ca.pem");
    EXPECT_TRUE(read[2].verifyPeer);
    EXPECT_EQ(read[2].codec, "cbor");
}

TEST_F(DaemonStateTest, RuntimeState_SslRoundTrip)
{
    DaemonRuntimeState s = minimalState("instSsl");
    s.resolved.sslCert = "/etc/ssl/cert.pem";
    s.resolved.sslKey  = "/etc/ssl/key.pem";
    s.resolved.sslCa   = "/etc/ssl/ca.pem";
    ASSERT_TRUE(DaemonRuntimeStateFile::write(s));

    DaemonRuntimeState got = DaemonRuntimeStateFile::read();
    EXPECT_EQ(got.resolved.sslCert, "/etc/ssl/cert.pem");
    EXPECT_EQ(got.resolved.sslKey,  "/etc/ssl/key.pem");
    EXPECT_EQ(got.resolved.sslCa,   "/etc/ssl/ca.pem");
}

// -- DaemonConfigFile (config.json) ---------------------------------------

TEST_F(DaemonStateTest, Config_ReadReturnsNulloptWhenFileMissing)
{
    EXPECT_FALSE(DaemonConfigFile::read().has_value());
}

TEST_F(DaemonStateTest, Config_RoundTripsEveryField)
{
    DaemonConfig cfg = sampleConfig();
    ASSERT_TRUE(DaemonConfigFile::write(cfg));

    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->modulesDirs.size(), 2u);
    EXPECT_EQ(got->loadModules, "mod1,mod2");
    EXPECT_EQ(got->persistencePath, "/var/lib/logoscore");
    EXPECT_EQ(got->modules.size(), 2u);
    EXPECT_EQ(got->modules.at("core_service").back().port, 6001);
    EXPECT_EQ(got->sslCert, "/etc/ssl/cert.pem");
    EXPECT_EQ(got->sslKey,  "/etc/ssl/key.pem");
    EXPECT_EQ(got->sslCa,   "/etc/ssl/ca.pem");
    EXPECT_TRUE(got->insecureTcp);
}

TEST_F(DaemonStateTest, Config_PreservesPortZeroIntent)
{
    // The whole point of separating config.json from state.json is
    // that operator intent (port=0 = "auto-pick") survives the
    // serialization round-trip; resolved-port lives in state.json.
    DaemonConfig cfg;
    cfg.modules["core_service"] = {{"tcp", "127.0.0.1", 0, "", true, "json"}};
    ASSERT_TRUE(DaemonConfigFile::write(cfg));

    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->modules.at("core_service").front().port, 0);
}

TEST_F(DaemonStateTest, Config_RejectsUnknownSchemaVersion)
{
    fs::path p(DaemonConfigFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p) << R"({"version":99})" << "\n";
    EXPECT_FALSE(DaemonConfigFile::read().has_value());
}
