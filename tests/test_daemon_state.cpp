#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
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

        Config::setConfigDir("");
    }

    void TearDown() override {
        setenv("HOME", origHome.c_str(), 1);
        if (origConfigDirSet)
            setenv("LOGOSCORE_CONFIG_DIR", origConfigDir.c_str(), 1);
        else
            unsetenv("LOGOSCORE_CONFIG_DIR");
        Config::setConfigDir("");
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
    cfg.persistencePath = "/var/lib/logoscore";
    cfg.modules["core_service"]      = {{"local"}, {"tcp", "127.0.0.1", 6001, "", true, "json"}};
    cfg.modules["capability_module"] = {{"local"}};
    cfg.sslCert = "/etc/ssl/cert.pem";
    cfg.sslKey  = "/etc/ssl/key.pem";
    cfg.sslCa   = "/etc/ssl/ca.pem";
    cfg.insecureTcp = true;
    cfg.accessPolicy =
        R"({"version":1,"mode":"enforce","restrictions":)"
        R"({"package_manager":{"allowedCallers":["package_manager_ui"]}}})";
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
    EXPECT_EQ(got->persistencePath, "/var/lib/logoscore");
    EXPECT_EQ(got->modules.size(), 2u);
    EXPECT_EQ(got->modules.at("core_service").back().port, 6001);
    EXPECT_EQ(got->sslCert, "/etc/ssl/cert.pem");
    EXPECT_EQ(got->sslKey,  "/etc/ssl/key.pem");
    EXPECT_EQ(got->sslCa,   "/etc/ssl/ca.pem");
    EXPECT_TRUE(got->insecureTcp);
    EXPECT_EQ(got->accessPolicy, sampleConfig().accessPolicy);
}

TEST_F(DaemonStateTest, Config_OmitsAccessPolicyWhenEmpty)
{
    DaemonConfig cfg = sampleConfig();
    cfg.accessPolicy.clear();
    ASSERT_TRUE(DaemonConfigFile::write(cfg));

    // Empty policy is not serialized (the key is omitted), and reads
    // back as empty rather than as a stray "" entry.
    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->accessPolicy.empty());
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

// -- writeLocalClientArtifacts (client/config.json) -----------------------

namespace {

std::string slurp(const fs::path& p)
{
    std::ifstream ifs(p);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

const std::vector<TransportInfo> kLocalOnly = { TransportInfo{"local"} };

fs::path clientCfgPath()
{
    return fs::path(Config::clientConfigPath());
}

bool writeArtifacts(const std::string& instanceId, bool freshTokensFile)
{
    return DaemonRuntimeStateFile::writeLocalClientArtifacts(
        instanceId, "raw-token", currentUtcIso8601(), freshTokensFile,
        kLocalOnly, kLocalOnly);
}

} // namespace

TEST_F(DaemonStateTest, ClientArtifacts_WritesConfigOnFreshBoot)
{
    ASSERT_FALSE(fs::exists(clientCfgPath()));
    EXPECT_TRUE(writeArtifacts("inst-A", /*freshTokensFile=*/true));
    ASSERT_TRUE(fs::exists(clientCfgPath()));
    EXPECT_NE(slurp(clientCfgPath()).find("inst-A"), std::string::npos);
}

TEST_F(DaemonStateTest, ClientArtifacts_SkipsConfigWhenAbsentAndNotFresh)
{
    EXPECT_TRUE(writeArtifacts("inst-A", /*freshTokensFile=*/false));
    EXPECT_FALSE(fs::exists(clientCfgPath()));
}

TEST_F(DaemonStateTest, ClientArtifacts_RefreshesStaleInstanceId)
{
    fs::create_directories(clientCfgPath().parent_path());
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"auto.json","instance_id":"OLD","daemon":{}})"
        << "\n";

    // Persisted config dir, replaced daemon: the existing file points
    // at a different instance, so even a non-fresh boot must refresh it.
    EXPECT_TRUE(writeArtifacts("NEW", /*freshTokensFile=*/false));

    const std::string body = slurp(clientCfgPath());
    EXPECT_NE(body.find("NEW"), std::string::npos);
    EXPECT_EQ(body.find("OLD"), std::string::npos);
}

TEST_F(DaemonStateTest, ClientArtifacts_LeavesMatchingInstanceIdUntouched)
{
    fs::create_directories(clientCfgPath().parent_path());
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"auto.json","instance_id":"SAME","custom":"keep"})"
        << "\n";

    EXPECT_TRUE(writeArtifacts("SAME", /*freshTokensFile=*/true));

    // Instance already matches: no rewrite, operator's field survives.
    EXPECT_NE(slurp(clientCfgPath()).find(R"("custom":"keep")"),
              std::string::npos);
}

TEST_F(DaemonStateTest, ClientArtifacts_NeverClobbersOperatorRemoteConfig)
{
    fs::create_directories(clientCfgPath().parent_path());
    // Operator-authored remote config: no instance_id field at all.
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"my.json","daemon":{"core_service":{"transport":"tcp","host":"10.0.0.5","port":6000}}})"
        << "\n";

    EXPECT_TRUE(writeArtifacts("inst-Z", /*freshTokensFile=*/true));

    const std::string body = slurp(clientCfgPath());
    EXPECT_NE(body.find("10.0.0.5"), std::string::npos);
    EXPECT_EQ(body.find("inst-Z"), std::string::npos);
}
