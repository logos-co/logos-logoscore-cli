#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <nlohmann/json.hpp>
#include "client/client_state.h"
#include "daemon/daemon_state.h"
#include "config.h"
#include "yaml_json.h"

namespace fs = std::filesystem;

class DaemonStateTest : public ::testing::Test {
protected:
    std::string origHome;
    std::string origConfigDir;
    bool        origConfigDirSet = false;
    std::string testDir;

    void SetUp() override {
        testDir = (fs::temp_directory_path() / ("logosctl_test_state_" + std::to_string(getpid()))).string();
        fs::create_directories(testDir + "/.logosctl");

        // Cover all three layers Config::configDir() consults so the
        // tests can never escape into the user's real ~/.logosctl.
        const char* home = std::getenv("HOME");
        origHome = home ? home : "";
        setenv("HOME", testDir.c_str(), 1);

        const char* cd = std::getenv("LOGOSCTL_CONFIG_DIR");
        origConfigDirSet = cd != nullptr;
        origConfigDir = origConfigDirSet ? cd : "";
        unsetenv("LOGOSCTL_CONFIG_DIR");

        Config::setConfigDir("");
    }

    void TearDown() override {
        setenv("HOME", origHome.c_str(), 1);
        if (origConfigDirSet)
            setenv("LOGOSCTL_CONFIG_DIR", origConfigDir.c_str(), 1);
        else
            unsetenv("LOGOSCTL_CONFIG_DIR");
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
    cfg.persistencePath = "/var/lib/logosctl";
    cfg.modules["core_service"]      = {{"local"}, {"tcp", "127.0.0.1", 6001, "", true, "json"}};
    cfg.modules["capability_module"] = {{"local"}};
    cfg.sslCert = "/etc/ssl/cert.pem";
    cfg.sslKey  = "/etc/ssl/key.pem";
    cfg.sslCa   = "/etc/ssl/ca.pem";
    cfg.insecureTcp = true;
    cfg.accessPolicy =
        R"({"version":1,"mode":"enforce","restrictions":)"
        R"({"package_manager":{"allowedCallers":["package_manager_ui"]}}})";
    cfg.signaturePolicy = "require";
    return cfg;
}

// A config whose only TLS material is the top-level `ssl:` block: two
// listeners, neither naming its own cert/key/ca.
DaemonConfig tlsConfigWithTopLevelSslOnly()
{
    DaemonConfig cfg;
    cfg.sslCert = "/etc/ssl/cert.pem";
    cfg.sslKey  = "/etc/ssl/key.pem";
    cfg.sslCa   = "/etc/ssl/ca.pem";
    cfg.modules["core_service"] =
        {{"local"}, {"tcp_ssl", "0.0.0.0", 8645, "", true, "json"}};
    cfg.modules["capability_module"] =
        {{"tcp_ssl", "0.0.0.0", 8646, "", true, "json"}};
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
    s.resolved.persistencePath = "/var/lib/logosctl";
    ASSERT_TRUE(DaemonRuntimeStateFile::write(s));

    DaemonRuntimeState got = DaemonRuntimeStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.schemaVersion, kDaemonRuntimeStateSchemaVersion);
    EXPECT_EQ(got.instanceId, "inst123");
    EXPECT_EQ(got.pid, getpid());
    EXPECT_EQ(got.configSource, "cli");
    EXPECT_EQ(got.resolved.modulesDirs.size(), 2u);
    EXPECT_EQ(got.resolved.modulesDirs[0], "/path/a");
    EXPECT_EQ(got.resolved.persistencePath, "/var/lib/logosctl");
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
    EXPECT_EQ(got->persistencePath, "/var/lib/logosctl");
    EXPECT_EQ(got->modules.size(), 2u);
    EXPECT_EQ(got->modules.at("core_service").back().port, 6001);
    EXPECT_EQ(got->sslCert, "/etc/ssl/cert.pem");
    EXPECT_EQ(got->sslKey,  "/etc/ssl/key.pem");
    EXPECT_EQ(got->sslCa,   "/etc/ssl/ca.pem");
    EXPECT_TRUE(got->insecureTcp);
    EXPECT_EQ(got->accessPolicy, sampleConfig().accessPolicy);
    EXPECT_EQ(got->signaturePolicy, "require");
}

// -- signature_policy ------------------------------------------------------
//
// The key was on the `daemon config set` allowlist, stored, and echoed back by
// `daemon config show`, but nothing read it: an operator who asked for
// `require` got the module's default `warn` and no warning that the setting
// did nothing. It is now carried on DaemonConfig and pushed into
// package_manager at boot (daemon.cpp's bootstrapPackageModules), so these
// tests pin the half that decides what reaches the module.

TEST_F(DaemonStateTest, Config_SignaturePolicyRoundTripsEachAcceptedValue)
{
    for (const char* policy : {"none", "warn", "require"}) {
        DaemonConfig cfg;
        cfg.signaturePolicy = policy;
        ASSERT_TRUE(DaemonConfigFile::write(cfg)) << policy;

        auto got = DaemonConfigFile::read();
        ASSERT_TRUE(got.has_value()) << policy;
        EXPECT_EQ(got->signaturePolicy, policy);
    }
}

TEST_F(DaemonStateTest, Config_UnsetSignaturePolicyStaysEmpty)
{
    // Empty means "say nothing to package_manager and let it keep its own
    // default". It must not round-trip into a "" that the reader then rejects.
    DaemonConfig cfg;
    ASSERT_TRUE(DaemonConfigFile::write(cfg));

    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_TRUE(got->signaturePolicy.empty());
}

TEST_F(DaemonStateTest, Config_RejectsUnknownSignaturePolicy)
{
    // package_manager ignores a policy string it does not recognise, so a
    // near-miss like `required` would leave it on `warn` while the config kept
    // displaying the operator's stricter intent. Fail the load instead.
    fs::path p(DaemonConfigFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p) << R"({"version":)" << kDaemonConfigSchemaVersion
                     << R"(,"signature_policy":"required"})" << "\n";
    EXPECT_FALSE(DaemonConfigFile::read().has_value());
}

TEST_F(DaemonStateTest, SignaturePolicy_AllowlistIsExactlyTheDocumentedThree)
{
    EXPECT_TRUE(isValidSignaturePolicy("none"));
    EXPECT_TRUE(isValidSignaturePolicy("warn"));
    EXPECT_TRUE(isValidSignaturePolicy("require"));
    for (const char* bad : {"", "required", "strict", "Require", "REQUIRE", "all"})
        EXPECT_FALSE(isValidSignaturePolicy(bad)) << bad;
}

// -- top-level ssl: defaults ----------------------------------------------
//
// The block was parsed into DaemonConfig::sslCert/sslKey/sslCa and read by
// nobody: only per-listener cert/key reached the transport set, so configuring
// TLS the obvious way produced listeners with no certificate and a handshake
// that died with "no shared cipher". It is now a default that per-listener
// values override.

TEST_F(DaemonStateTest, SslDefaults_FillListenersThatNameNoMaterial)
{
    DaemonConfig cfg = tlsConfigWithTopLevelSslOnly();
    applySslDefaults(cfg);

    const auto& core = cfg.modules.at("core_service");
    ASSERT_EQ(core.size(), 2u);
    // The local entry is untouched -- it has nowhere to put a certificate.
    EXPECT_TRUE(core[0].certFile.empty());
    EXPECT_TRUE(core[0].keyFile.empty());
    EXPECT_EQ(core[1].certFile, "/etc/ssl/cert.pem");
    EXPECT_EQ(core[1].keyFile,  "/etc/ssl/key.pem");
    EXPECT_EQ(core[1].caFile,   "/etc/ssl/ca.pem");

    // Every tcp_ssl listener is covered, not just the first module.
    const auto& cap = cfg.modules.at("capability_module");
    ASSERT_EQ(cap.size(), 1u);
    EXPECT_EQ(cap[0].certFile, "/etc/ssl/cert.pem");
    EXPECT_EQ(cap[0].keyFile,  "/etc/ssl/key.pem");
}

TEST_F(DaemonStateTest, SslDefaults_PerListenerMaterialWins)
{
    // A default that overrode what the listener named would make per-listener
    // certs unusable the moment a top-level block existed.
    DaemonConfig cfg = tlsConfigWithTopLevelSslOnly();
    auto& core = cfg.modules.at("core_service");
    core[1].certFile = "/own/cert.pem";
    core[1].keyFile  = "/own/key.pem";
    core[1].caFile   = "/own/ca.pem";
    applySslDefaults(cfg);

    EXPECT_EQ(core[1].certFile, "/own/cert.pem");
    EXPECT_EQ(core[1].keyFile,  "/own/key.pem");
    EXPECT_EQ(core[1].caFile,   "/own/ca.pem");
    // ...and the listener that named nothing still inherits.
    EXPECT_EQ(cfg.modules.at("capability_module")[0].certFile, "/etc/ssl/cert.pem");
}

TEST_F(DaemonStateTest, SslDefaults_FillPerFieldNotPerListener)
{
    // A listener that names only its cert still inherits the key: the merge is
    // field-by-field, so a half-specified listener is completed rather than
    // left to bind with a cert and no key.
    DaemonConfig cfg = tlsConfigWithTopLevelSslOnly();
    auto& core = cfg.modules.at("core_service");
    core[1].certFile = "/own/cert.pem";
    applySslDefaults(cfg);

    EXPECT_EQ(core[1].certFile, "/own/cert.pem");
    EXPECT_EQ(core[1].keyFile,  "/etc/ssl/key.pem");
}

TEST_F(DaemonStateTest, SslDefaults_LeaveNonTlsTransportsAlone)
{
    DaemonConfig cfg;
    cfg.sslCert = "/etc/ssl/cert.pem";
    cfg.sslKey  = "/etc/ssl/key.pem";
    cfg.modules["core_service"] =
        {{"local"}, {"tcp", "127.0.0.1", 6001, "", true, "json"}};
    applySslDefaults(cfg);

    for (const auto& t : cfg.modules.at("core_service")) {
        EXPECT_TRUE(t.certFile.empty()) << t.protocol;
        EXPECT_TRUE(t.keyFile.empty())  << t.protocol;
    }
}

TEST_F(DaemonStateTest, SslDefaults_NoBlockIsANoOp)
{
    DaemonConfig cfg;
    cfg.modules["core_service"] = {{"tcp_ssl", "0.0.0.0", 8645, "", true, "json"}};
    applySslDefaults(cfg);
    EXPECT_TRUE(cfg.modules.at("core_service")[0].certFile.empty());
}

TEST_F(DaemonStateTest, SslDefaults_SurviveTheConfigRoundTrip)
{
    // End to end over the real reader: a document whose only TLS material is
    // the top-level block yields listeners that carry it.
    DaemonConfig cfg = tlsConfigWithTopLevelSslOnly();
    ASSERT_TRUE(DaemonConfigFile::write(cfg));

    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    ASSERT_FALSE(findTlsListenersMissingMaterial(*got).empty())
        << "Pre-condition: the listeners start with no material of their own.";
    applySslDefaults(*got);
    EXPECT_TRUE(findTlsListenersMissingMaterial(*got).empty());
    EXPECT_EQ(got->modules.at("capability_module")[0].certFile, "/etc/ssl/cert.pem");
}

TEST_F(DaemonStateTest, MissingTlsMaterial_NamesEveryCertlessListener)
{
    DaemonConfig cfg = tlsConfigWithTopLevelSslOnly();
    cfg.sslCert.clear();
    cfg.sslKey.clear();
    applySslDefaults(cfg);

    auto missing = findTlsListenersMissingMaterial(cfg);
    ASSERT_EQ(missing.size(), 2u);
    // The message has to identify which listener to go fix.
    EXPECT_NE(missing[0].find("capability_module"), std::string::npos) << missing[0];
    EXPECT_NE(missing[0].find("8646"), std::string::npos) << missing[0];
    EXPECT_NE(missing[1].find("core_service"), std::string::npos) << missing[1];
}

TEST_F(DaemonStateTest, MissingTlsMaterial_KeyWithoutCertIsStillMissing)
{
    DaemonConfig cfg;
    cfg.modules["core_service"] =
        {{"local"}, {"tcp", "127.0.0.1", 6001, "", true, "json"},
         {"tcp_ssl", "0.0.0.0", 8645, "", true, "json", "/own/cert.pem", ""}};
    // local and plaintext tcp never need TLS material; the half-specified
    // tcp_ssl listener does.
    auto missing = findTlsListenersMissingMaterial(cfg);
    ASSERT_EQ(missing.size(), 1u);
    EXPECT_NE(missing[0].find("core_service"), std::string::npos) << missing[0];
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

bool writeArtifacts(const std::string& instanceId,
                    const std::string& accessGroup = {})
{
    return DaemonRuntimeStateFile::writeLocalClientArtifacts(
        instanceId, "raw-token", currentUtcIso8601(),
        kLocalOnly, kLocalOnly, accessGroup);
}

} // namespace

TEST_F(DaemonStateTest, ClientArtifacts_WritesConfigWhenMissing)
{
    ASSERT_FALSE(fs::exists(clientCfgPath()));
    // A missing config.json is always (re)generated — the client's only
    // channel for the per-boot instance_id, so it must reappear whether this
    // is a first boot or a persisted dir that lost the file.
    EXPECT_TRUE(writeArtifacts("inst-A"));
    ASSERT_TRUE(fs::exists(clientCfgPath()));
    EXPECT_NE(slurp(clientCfgPath()).find("inst-A"), std::string::npos);
}

TEST_F(DaemonStateTest, ClientArtifacts_RefreshesStaleInstanceIdPreservingTokenFile)
{
    fs::create_directories(clientCfgPath().parent_path());
    // Operator repointed token_file away from auto.json, then the daemon was
    // replaced (stale instance_id). The refresh must update instance_id but
    // keep the operator's token_file.
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"alice.json","instance_id":"OLD","daemon":{}})"
        << "\n";

    EXPECT_TRUE(writeArtifacts("NEW"));

    const std::string body = slurp(clientCfgPath());
    EXPECT_NE(body.find("NEW"), std::string::npos);
    EXPECT_EQ(body.find("OLD"), std::string::npos);
    EXPECT_NE(body.find("alice.json"), std::string::npos);
}

TEST_F(DaemonStateTest, ClientArtifacts_LeavesMatchingInstanceIdUntouched)
{
    fs::create_directories(clientCfgPath().parent_path());
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"auto.json","instance_id":"SAME","custom":"keep"})"
        << "\n";

    EXPECT_TRUE(writeArtifacts("SAME"));

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

    EXPECT_TRUE(writeArtifacts("inst-Z"));

    const std::string body = slurp(clientCfgPath());
    EXPECT_NE(body.find("10.0.0.5"), std::string::npos);
    EXPECT_EQ(body.find("inst-Z"), std::string::npos);
}

TEST_F(DaemonStateTest, ClientArtifacts_OwnerOnlyByDefault)
{
    ASSERT_TRUE(writeArtifacts("inst-A"));
    struct stat st;
    ASSERT_EQ(::stat(Config::clientTokenPath("auto.json").c_str(), &st), 0);
    // No --access-group: the raw token file stays 0600.
    EXPECT_EQ(st.st_mode & 07777, 0600u);
}

TEST_F(DaemonStateTest, ClientArtifacts_GroupReadableWithAccessGroup)
{
    // Pass our own effective gid (as a numeric group spec) so the chgrp always
    // succeeds in the sandbox. config.json + auto.json must become 0640 and
    // owned by that group so a member can read them.
    const std::string gid = std::to_string(::getegid());
    ASSERT_TRUE(writeArtifacts("inst-A", gid));

    struct stat cfgSt;
    ASSERT_EQ(::stat(Config::clientConfigPath().c_str(), &cfgSt), 0);
    EXPECT_EQ(cfgSt.st_mode & 07777, 0640u);
    EXPECT_EQ(cfgSt.st_gid, ::getegid());

    struct stat tokSt;
    ASSERT_EQ(::stat(Config::clientTokenPath("auto.json").c_str(), &tokSt), 0);
    EXPECT_EQ(tokSt.st_mode & 07777, 0640u);
    EXPECT_EQ(tokSt.st_gid, ::getegid());
}

// ── tcp_ssl cert/key ─────────────────────────────────────────────────────────
//
// The parser did not read `cert`/`key` at all. It never mattered while those
// came from --module-transport on the command line, but once the config file
// became the only place to set transports, every tcp_ssl listener bound with
// no certificate: the daemon started, accepted connections, and failed every
// handshake with "no shared cipher". TLS was unconfigurable and nothing said
// so.
TEST_F(DaemonStateTest, Config_RoundTripsTlsCertAndKey)
{
    DaemonConfig cfg;
    TransportInfo t;
    t.protocol = "tcp_ssl";
    t.host     = "127.0.0.1";
    t.port     = 6443;
    t.certFile = "/etc/logos/server.pem";
    t.keyFile  = "/etc/logos/server.key";
    t.caFile   = "/etc/logos/ca.pem";
    cfg.modules["core_service"] = { t };

    ASSERT_TRUE(DaemonConfigFile::write(cfg));
    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->modules.count("core_service"), 1u);
    ASSERT_EQ(got->modules["core_service"].size(), 1u);

    const TransportInfo& r = got->modules["core_service"][0];
    EXPECT_EQ(r.protocol, "tcp_ssl");
    EXPECT_EQ(r.port,     6443);
    EXPECT_EQ(r.certFile, "/etc/logos/server.pem")
        << "the server certificate must survive a config round-trip";
    EXPECT_EQ(r.keyFile,  "/etc/logos/server.key")
        << "the server key must survive a config round-trip";
    EXPECT_EQ(r.caFile,   "/etc/logos/ca.pem");
}

// state.json is a runtime record clients read, so the key path stays out of
// it -- the config file is where that secret belongs.
TEST_F(DaemonStateTest, RuntimeState_OmitsTlsCertAndKey)
{
    DaemonRuntimeState s = minimalState("inst-tls");
    TransportInfo t;
    t.protocol = "tcp_ssl";
    t.host     = "127.0.0.1";
    t.port     = 6443;
    t.certFile = "/etc/logos/server.pem";
    t.keyFile  = "/etc/logos/server.key";
    t.caFile   = "/etc/logos/ca.pem";
    s.resolved.modules["core_service"] = { t };
    ASSERT_TRUE(DaemonRuntimeStateFile::write(s));

    std::ifstream in(Config::daemonStatePath());
    ASSERT_TRUE(in.good());
    const std::string raw((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    EXPECT_EQ(raw.find("server.key"), std::string::npos)
        << "state.json must not carry the server's private key path";
    EXPECT_EQ(raw.find("server.pem"), std::string::npos)
        << "state.json must not carry the server's certificate path";
    // The CA is client-relevant, so it does belong here.
    EXPECT_NE(raw.find("ca.pem"), std::string::npos);
}

// ── Type-mismatched values ───────────────────────────────────────────────────
//
// The reader used nlohmann's `json::value(key, default)`, which THROWS
// `json::type_error` when the key is present carrying a different type than
// the default. Nothing caught it, so `modules_dirs: /single/path` -- a scalar
// where a list belongs, i.e. an ordinary typo -- terminated the process:
//
//   libc++abi: terminating due to uncaught exception of type
//   nlohmann::detail::type_error: [json.exception.type_error.302]
//   type must be array, but is string
//
// Every one of these must instead come back as a normal error naming the
// offending key. EXPECT_NO_THROW is what separates "rejected" from "aborted":
// without it the uncaught throw takes the test binary down with it.

namespace {

using nlohmann::json;

// Install raw document text at the daemon config path, bypassing the writer,
// the way a hand-edited file arrives.
void writeDaemonConfigText(const std::string& text)
{
    fs::path p(DaemonConfigFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << text;
}

// Validate a document and return the rejection message ("" when accepted).
std::string daemonConfigError(const json& doc)
{
    std::string err;
    auto cfg = parseDaemonConfigDocument(doc, &err);
    return cfg.has_value() ? std::string{} : err;
}

std::string clientConfigError(const json& doc)
{
    std::string err;
    auto state = parseClientStateDocument(doc, &err);
    return state.has_value() ? std::string{} : err;
}

// An error is only actionable if it names the key the operator has to fix.
::testing::AssertionResult namesKey(const std::string& error,
                                    const std::string& key)
{
    if (error.empty())
        return ::testing::AssertionFailure()
            << "expected the document to be rejected, but it was accepted";
    if (error.find(key) == std::string::npos)
        return ::testing::AssertionFailure()
            << "the error should name `" << key << "`, but it reads: " << error;
    return ::testing::AssertionSuccess();
}

} // namespace

TEST_F(DaemonStateTest, Config_ScalarWhereAListBelongsIsRejectedNotFatal)
{
    // The reported repro, through the same path the daemon reads at boot.
    writeDaemonConfigText("version: 2\nmodules_dirs: /single/path\n");

    std::optional<DaemonConfig> got;
    ASSERT_NO_THROW({ got = DaemonConfigFile::read(); })
        << "a type-mismatched value must not abort the process";
    EXPECT_FALSE(got.has_value())
        << "a config that cannot be understood must not load";
}

TEST_F(DaemonStateTest, Config_TypeMismatchNamesTheOffendingKey)
{
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"modules_dirs", "/single/path"}}),
        "modules_dirs"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"persistence_path", 42}}),
        "persistence_path"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"access_policy", {{"mode", "enforce"}}}}),
        "access_policy"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"insecure_tcp", "yes"}}),
        "insecure_tcp"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"modules_dirs", {"/ok", 7}}}),
        "modules_dirs[1]"));
    // A version that isn't a number must be reported, not thrown on, and not
    // silently read as "version 0".
    EXPECT_TRUE(namesKey(daemonConfigError({{"version", "two"}}), "version"));
}

TEST_F(DaemonStateTest, Config_TypeMismatchSaysWhatWasExpected)
{
    const std::string err =
        daemonConfigError({{"version", 2}, {"modules_dirs", "/single/path"}});
    EXPECT_NE(err.find("a list of strings"), std::string::npos)
        << "the error should say what the key takes. It reads: " << err;
    EXPECT_NE(err.find("a string"), std::string::npos)
        << "the error should say what was found instead. It reads: " << err;
}

TEST_F(DaemonStateTest, Config_TypeMismatchInNestedBlocksIsRejected)
{
    // Nested keys are named by their full path, so "file" alone can't be
    // confused with some other `file` elsewhere in the document.
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"logging", {{"file", 42}}}}),
        "logging.file"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"logging", {{"max_size_mb", "big"}}}}),
        "logging.max_size_mb"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"dirs", {{"keyring", true}}}}),
        "dirs.keyring"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"ssl", {{"cert", 1}}}}),
        "ssl.cert"));
    // A whole block of the wrong type, too -- ignoring it would drop the
    // operator's intent with nothing to explain it.
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"ssl", "/etc/ssl/cert.pem"}}),
        "ssl"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"logging", true}}),
        "logging"));
}

TEST_F(DaemonStateTest, Config_TypeMismatchInsideATransportIsRejected)
{
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"modules", "core_service"}}),
        "modules"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2}, {"modules", {{"core_service", "tcp"}}}}),
        "modules.core_service"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service", {{"transports", "tcp"}}}}}}),
        "modules.core_service.transports"));
    // A port typed as a string used to throw; an out-of-range one was rejected
    // with no indication of which entry was at fault.
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service",
                             json::array({{{"protocol", "tcp"}, {"port", "6001"}}})}}}}),
        "modules.core_service.transports[0].port"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service",
                             json::array({{{"protocol", "tcp"}, {"port", 70000}}})}}}}),
        "modules.core_service.transports[0].port"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service",
                             json::array({{{"protocol", "tcp"}, {"verify_peer", "no"}}})}}}}),
        "modules.core_service.transports[0].verify_peer"));
    // The pre-existing strict-allowlist rejection now says which entry it is.
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service",
                             json::array({{{"protocol", "tcpp"}}})}}}}),
        "modules.core_service.transports[0].protocol"));
}

TEST_F(DaemonStateTest, Config_EmptyValueMeansUnsetNotMistyped)
{
    // `key:` with nothing after it is YAML null. It means "not set" -- the
    // reader's defaults apply -- and must not be reported as a type error.
    writeDaemonConfigText("version: 2\n"
                          "modules_dirs:\n"
                          "access_policy:\n"
                          "modules:\n"
                          "  core_service:\n");
    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value()) << "an empty value is not a type mismatch";
    EXPECT_TRUE(got->modulesDirs.empty());
    EXPECT_TRUE(got->accessPolicy.empty());
    EXPECT_TRUE(got->modules.empty());
}

TEST_F(DaemonStateTest, Config_WellFormedDocumentStillLoads)
{
    // The guard rejects mistyped values, not ordinary ones.
    writeDaemonConfigText("version: 2\n"
                          "modules_dirs:\n"
                          "  - /opt/modules\n"
                          "insecure_tcp: true\n"
                          "logging:\n"
                          "  max_size_mb: 25\n"
                          "  console: false\n"
                          "modules:\n"
                          "  core_service:\n"
                          "    - protocol: tcp\n"
                          "      host: 127.0.0.1\n"
                          "      port: 8645\n");
    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->modulesDirs, std::vector<std::string>{"/opt/modules"});
    EXPECT_TRUE(got->insecureTcp);
    EXPECT_EQ(got->logging.maxSizeMb, 25u);
    EXPECT_FALSE(got->logging.console);
    EXPECT_EQ(got->modules.at("core_service").front().port, 8645);
}

TEST_F(DaemonStateTest, RuntimeState_TypeMismatchIsRejectedNotFatal)
{
    // state.json is machine-written, so a mismatch here means a corrupted or
    // hand-edited file. It must read as "no usable state", never as a crash.
    fs::path p(DaemonRuntimeStateFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc)
        << R"({"version":2,"instance_id":"x","pid":"not-a-pid"})" << "\n";

    DaemonRuntimeState got;
    ASSERT_NO_THROW({ got = DaemonRuntimeStateFile::read(); });
    EXPECT_FALSE(got.fileOk);
}

TEST_F(DaemonStateTest, ClientArtifacts_MistypedExistingConfigDoesNotAbortBoot)
{
    // This runs during daemon startup, against a file the operator may have
    // edited. Reading `instance_id` with the wrong type used to throw here,
    // taking the whole boot down.
    fs::create_directories(clientCfgPath().parent_path());
    std::ofstream(clientCfgPath(), std::ios::trunc)
        << R"({"version":2,"token_file":42,"instance_id":7,"daemon":{}})" << "\n";

    bool wrote = false;
    ASSERT_NO_THROW({ wrote = writeArtifacts("inst-A"); });
    EXPECT_TRUE(wrote);
}

// ── The client half of the same hazard ───────────────────────────────────────

TEST_F(DaemonStateTest, ClientConfig_TypeMismatchNamesTheOffendingKey)
{
    EXPECT_TRUE(namesKey(
        clientConfigError({{"version", 2}, {"token_file", 42}}),
        "token_file"));
    EXPECT_TRUE(namesKey(
        clientConfigError({{"version", 2}, {"instance_id", {{"a", 1}}}}),
        "instance_id"));
    EXPECT_TRUE(namesKey(
        clientConfigError({{"version", 2}, {"daemon", "core_service"}}),
        "daemon"));
    EXPECT_TRUE(namesKey(
        clientConfigError({{"version", 2},
                           {"daemon", {{"core_service",
                             {{"transport", "tcp"}, {"port", "6001"}}}}}}),
        "daemon.core_service.port"));
    EXPECT_TRUE(namesKey(
        clientConfigError({{"version", 2},
                           {"daemon", {{"core_service",
                             {{"transport", "tcp_ssl"}, {"verify_peer", "yes"}}}}}}),
        "daemon.core_service.verify_peer"));
    EXPECT_TRUE(namesKey(clientConfigError({{"version", "two"}}), "version"));
}

TEST_F(DaemonStateTest, ClientConfig_TypeMismatchIsRejectedNotFatal)
{
    fs::path p(Config::clientConfigPath());
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << "version: 2\ntoken_file: 42\n";

    ClientState got;
    ASSERT_NO_THROW({ got = ClientStateFile::read(); })
        << "a type-mismatched value must not abort the process";
    EXPECT_FALSE(got.fileOk);
}

TEST_F(DaemonStateTest, ClientConfig_WellFormedDocumentStillLoads)
{
    fs::path p(Config::clientConfigPath());
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc)
        << "version: 2\n"
           "token_file: auto.json\n"
           "instance_id: inst-A\n"
           "daemon:\n"
           "  core_service:\n"
           "    transport: tcp\n"
           "    host: 10.0.0.5\n"
           "    port: 6001\n";

    ClientState got = ClientStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.tokenFile, "auto.json");
    EXPECT_EQ(got.instanceId, "inst-A");
    ASSERT_EQ(got.daemon.count("core_service"), 1u);
    EXPECT_EQ(got.daemon.at("core_service").host, "10.0.0.5");
    EXPECT_EQ(got.daemon.at("core_service").port, 6001);
}

TEST_F(DaemonStateTest, ClientConfig_MistypedTokenFieldIsNotAToken)
{
    // Same `value()` hazard on the token file, which is read on every client
    // command. A `token` of the wrong type reads as "no usable token".
    fs::create_directories(fs::path(Config::clientTokenPath("auto.json")).parent_path());
    std::ofstream(Config::clientTokenPath("auto.json"), std::ios::trunc)
        << R"({"version":1,"token":1234})" << "\n";

    std::string token = "unset";
    ASSERT_NO_THROW({ token = ClientStateFile::readTokenFile("auto.json"); });
    EXPECT_TRUE(token.empty());
}

// ── What was validated must be what lands on disk ────────────────────────────
//
// `config set` rewrites the operator's document through yaml_json::dump before
// writing it. yaml-cpp quotes only what would break the syntax, not what would
// change type, so a string like "6001" was emitted bare and came back as the
// number 6001 on the next read: the document that passed validation was not
// the document that landed on disk, and the mismatch surfaced later as a type
// error at boot.

TEST_F(DaemonStateTest, YamlRoundTrip_KeepsANumericLookingStringAString)
{
    for (const char* s : {"6001", "true", "false", "null", "1.5", "", "~"}) {
        const json doc = {{"value", s}};
        auto back = yaml_json::parse(yaml_json::dump(doc));
        ASSERT_TRUE(back.has_value()) << "`" << s << "` did not survive as YAML";
        ASSERT_TRUE((*back)["value"].is_string())
            << "`" << s << "` came back as " << (*back)["value"].dump();
        EXPECT_EQ((*back)["value"].get<std::string>(), s);
    }
}

TEST_F(DaemonStateTest, YamlRoundTrip_LeavesRealScalarsAlone)
{
    // The quoting is for strings that would be retyped, and nothing else: a
    // number stays a number, a bool stays a bool.
    const json doc = {{"port", 6001}, {"verify_peer", true}, {"codec", "json"}};
    auto back = yaml_json::parse(yaml_json::dump(doc));
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE((*back)["port"].is_number_integer());
    EXPECT_EQ((*back)["port"].get<int>(), 6001);
    EXPECT_TRUE((*back)["verify_peer"].is_boolean());
    EXPECT_EQ((*back)["codec"].get<std::string>(), "json");
}
