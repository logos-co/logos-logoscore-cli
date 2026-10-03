#include <gtest/gtest.h>
#include "test_platform.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "client/client_state.h"
#include "daemon/daemon_state.h"
#include "config.h"
#include "removed_transports.h"
#include "yaml_json.h"

namespace fs = std::filesystem;

class DaemonStateTest : public ::testing::Test {
protected:
    std::string origHome;
    std::string origConfigDir;
    bool        origConfigDirSet = false;
    std::string testDir;

    void SetUp() override {
        testDir = (fs::temp_directory_path() / ("logosctl_test_state_" + std::to_string(logosctl_test::currentPid()))).string();
        fs::create_directories(testDir + "/.logosctl");

        // Cover all three layers Config::configDir() consults so the
        // tests can never escape into the user's real ~/.logosctl.
        const char* home = std::getenv(logosctl_test::homeVar());
        origHome = home ? home : "";
        logosctl_test::setEnv(logosctl_test::homeVar(), testDir);

        const char* cd = std::getenv("LOGOSCTL_CONFIG_DIR");
        origConfigDirSet = cd != nullptr;
        origConfigDir = origConfigDirSet ? cd : "";
        logosctl_test::unsetEnv("LOGOSCTL_CONFIG_DIR");

        Config::setConfigDir("");
    }

    void TearDown() override {
        logosctl_test::setEnv(logosctl_test::homeVar(), origHome);
        if (origConfigDirSet)
            logosctl_test::setEnv("LOGOSCTL_CONFIG_DIR", origConfigDir);
        else
            logosctl_test::unsetEnv("LOGOSCTL_CONFIG_DIR");
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
    s.pid         = logosctl_test::currentPid();
    s.startedAt   = currentUtcIso8601();
    s.resolved.modulesDirs = dirs;
    return s;
}

DaemonConfig sampleConfig()
{
    DaemonConfig cfg;
    cfg.modulesDirs     = {"/path/a", "/path/b"};
    cfg.persistencePath = "/var/lib/logosctl";
    cfg.accessPolicy =
        R"({"version":1,"mode":"enforce","restrictions":)"
        R"({"package_manager":{"allowedCallers":["package_manager_ui"]}}})";
    cfg.signaturePolicy = "require";
    cfg.placement = R"({"default":"inproc"})";
    cfg.bundledModulesDirs = {"/opt/logos/modules"};
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
    EXPECT_EQ(got.pid, logosctl_test::currentPid());
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
    EXPECT_EQ(got->accessPolicy, sampleConfig().accessPolicy);
    EXPECT_EQ(got->signaturePolicy, "require");
    EXPECT_EQ(got->placement, sampleConfig().placement);
    EXPECT_EQ(got->bundledModulesDirs, std::vector<std::string>{"/opt/logos/modules"});
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

fs::path clientCfgPath()
{
    return fs::path(Config::clientConfigPath());
}

bool writeArtifacts(const std::string& instanceId,
                    const std::string& accessGroup = {})
{
    return DaemonRuntimeStateFile::writeLocalClientArtifacts(
        instanceId, "raw-token", currentUtcIso8601(), accessGroup);
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

TEST_F(DaemonStateTest, ClientArtifacts_RewrittenEveryBootKeepingTheTokenFile)
{
    // The daemon owns the dial spec; the operator owns token_file.
    fs::create_directories(clientCfgPath().parent_path());
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"alice.json","instance_id":"SAME","custom":"x"})"
        << "\n";

    EXPECT_TRUE(writeArtifacts("SAME"));

    const std::string body = slurp(clientCfgPath());
    EXPECT_EQ(body.find("custom"), std::string::npos) << body;
    EXPECT_NE(body.find("alice.json"), std::string::npos) << body;
}

TEST_F(DaemonStateTest, ClientArtifacts_ReplaceAnOldTcpDialSpec)
{
    // What a remote client used to hand-write: no instance_id, a tcp dial.
    fs::create_directories(clientCfgPath().parent_path());
    std::ofstream(clientCfgPath())
        << R"({"version":2,"token_file":"my.json","daemon":{"core_service":{"transport":"tcp","host":"10.0.0.5","port":6000}}})"
        << "\n";

    EXPECT_TRUE(writeArtifacts("inst-Z"));

    auto parsed = yaml_json::parse(slurp(clientCfgPath()));
    ASSERT_TRUE(parsed.has_value());
    std::string err;
    auto state = parseClientStateDocument(*parsed, &err);
    ASSERT_TRUE(state.has_value()) << err;
    EXPECT_EQ(state->instanceId, "inst-Z");
    EXPECT_EQ(state->tokenFile, "my.json");
    EXPECT_EQ(state->daemon, (std::set<std::string>{"capability_module", "core_service"}));
}

#ifndef _WIN32
// POSIX mode bits and groups. On Windows the files take their directory's ACL;
// chmodPosix documents that loss.
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
#endif

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
    // A whole block of the wrong type, too -- ignoring it would drop the
    // operator's intent with nothing to explain it.
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
        daemonConfigError({{"version", 2}, {"modules", {{"core_service", "local"}}}}),
        "modules.core_service"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service", {{"transports", "local"}}}}}}),
        "modules.core_service.transports"));
    EXPECT_TRUE(namesKey(
        daemonConfigError({{"version", 2},
                           {"modules", {{"core_service",
                             json::array({{{"protocol", 1}}})}}}}),
        "modules.core_service.transports[0].protocol"));
    // The strict-allowlist rejection says which entry it is.
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
}

TEST_F(DaemonStateTest, Config_WellFormedDocumentStillLoads)
{
    // The guard rejects mistyped values, not ordinary ones.
    writeDaemonConfigText("version: 2\n"
                          "modules_dirs:\n"
                          "  - /opt/modules\n"
                          "logging:\n"
                          "  max_size_mb: 25\n"
                          "  console: false\n"
                          "signature_policy: require\n");
    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->modulesDirs, std::vector<std::string>{"/opt/modules"});
    EXPECT_EQ(got->logging.maxSizeMb, 25u);
    EXPECT_FALSE(got->logging.console);
    EXPECT_EQ(got->signaturePolicy, "require");
}

// ── Settings removed with the tcp and tcp_ssl transports ─────────────────────
//
// Every config.json --persist-config wrote, and every state.json `resolved`
// block, carries `ssl` with empty paths, `insecure_tcp: false` and local
// listeners under `modules`. Those still load; in a config anything more is
// refused by name, never dropped. state.json drops the keys whatever they hold.

namespace {

// What --persist-config and state.json wrote for a local-only daemon.
json legacyDefaults()
{
    return {{"modules_dirs", json::array({"/opt/modules"})},
            {"modules", {{"core_service", {{"transports", json::array({{{"protocol", "local"}}})}}},
                         {"capability_module", {{"transports", json::array({{{"protocol", "local"}}})}}}}},
            {"ssl", {{"cert", ""}, {"key", ""}, {"ca", ""}}},
            {"insecure_tcp", false}};
}

::testing::AssertionResult refusedAsRemoved(const std::string& error, const std::string& key)
{
    ::testing::AssertionResult named = namesKey(error, "`" + key + "`");
    if (!named) return named;
    if (error.find("was removed with the tcp and tcp_ssl transports") == std::string::npos
        || error.find("logosctl remote pair") == std::string::npos)
        return ::testing::AssertionFailure() << "not the removal message: " << error;
    return ::testing::AssertionSuccess();
}

} // namespace

TEST_F(DaemonStateTest, RemovedKeys_TheirDefaultsStillLoad)
{
    json doc = legacyDefaults();
    doc["version"] = 2;
    std::string err;
    auto cfg = parseDaemonConfigDocument(doc, &err);
    ASSERT_TRUE(cfg.has_value()) << err;
    EXPECT_EQ(cfg->modulesDirs, std::vector<std::string>{"/opt/modules"});

    // The bare-list spelling of a local listener, and an empty block, too.
    doc["modules"] = {{"core_service", json::array({{{"protocol", "local"}}})}};
    doc["ssl"] = json::object();
    EXPECT_TRUE(parseDaemonConfigDocument(doc, &err).has_value()) << err;
}

TEST_F(DaemonStateTest, RemovedKeys_APersistedConfigJsonStillLoads)
{
    json doc = legacyDefaults();
    doc["version"] = 2;
    writeDaemonConfigText(doc.dump(4));
    auto got = DaemonConfigFile::read();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->modulesDirs, std::vector<std::string>{"/opt/modules"});
}

TEST_F(DaemonStateTest, RemovedKeys_AnOldStateJsonStillLoads)
{
    fs::path p(DaemonRuntimeStateFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc)
        << json{{"version", 2}, {"instance_id", "old123"}, {"pid", 4242},
                {"started_at", "2026-01-01T00:00:00Z"}, {"resolved", legacyDefaults()}}.dump(4);

    DaemonRuntimeState got = DaemonRuntimeStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.instanceId, "old123");
    EXPECT_EQ(got.resolved.modulesDirs, std::vector<std::string>{"/opt/modules"});
}

TEST_F(DaemonStateTest, RemovedKeys_AreNoLongerWritten)
{
    ASSERT_TRUE(DaemonConfigFile::write(sampleConfig()));
    auto config = yaml_json::parse(slurp(DaemonConfigFile::filePath()));
    ASSERT_TRUE(config.has_value());

    ASSERT_TRUE(DaemonRuntimeStateFile::write(minimalState("inst-w", {"/mods"})));
    const json state = json::parse(slurp(DaemonRuntimeStateFile::filePath()));

    for (const char* key : {"modules", "ssl", "insecure_tcp"}) {
        EXPECT_FALSE(config->contains(key)) << key << " in " << config->dump();
        EXPECT_FALSE(state["resolved"].contains(key)) << key << " in " << state.dump();
    }
}

TEST_F(DaemonStateTest, RemovedKeys_AreRefusedByName)
{
    const auto withListener = [](json listener) {
        return json{{"version", 2},
                    {"modules", {{"core_service", {{"transports", json::array({listener})}}}}}};
    };
    EXPECT_TRUE(refusedAsRemoved(
        daemonConfigError(withListener({{"protocol", "tcp"}, {"port", 6000}})),
        "modules.core_service.transports[0].protocol: tcp"));
    EXPECT_TRUE(refusedAsRemoved(
        daemonConfigError(withListener({{"protocol", "tcp_ssl"}})),
        "modules.core_service.transports[0].protocol: tcp_ssl"));
    for (const char* field : {"host", "port", "codec", "ca_file", "verify_peer", "cert", "key"}) {
        EXPECT_TRUE(refusedAsRemoved(
            daemonConfigError(withListener({{"protocol", "local"}, {field, "x"}})),
            std::string("modules.core_service.transports[0].") + field));
    }
    for (const char* field : {"cert", "key", "ca"}) {
        EXPECT_TRUE(refusedAsRemoved(
            daemonConfigError({{"version", 2}, {"ssl", {{field, "/etc/ssl/x.pem"}}}}),
            std::string("ssl.") + field));
    }
    EXPECT_TRUE(refusedAsRemoved(
        daemonConfigError({{"version", 2}, {"ssl", "/etc/ssl/cert.pem"}}), "ssl"));
    EXPECT_TRUE(refusedAsRemoved(
        daemonConfigError({{"version", 2}, {"insecure_tcp", true}}), "insecure_tcp"));
    EXPECT_TRUE(refusedAsRemoved(
        daemonConfigError({{"version", 2}, {"insecure_tcp", "no"}}), "insecure_tcp"));
}

TEST_F(DaemonStateTest, RemovedKeys_AreRefusedByTheLoaderToo)
{
    writeDaemonConfigText("version: 2\n"
                          "modules:\n"
                          "  core_service:\n"
                          "    - protocol: tcp\n"
                          "      host: 0.0.0.0\n");
    EXPECT_FALSE(DaemonConfigFile::read().has_value());
}

TEST_F(DaemonStateTest, RemovedKeys_AnOlderDaemonWithTcpListenersStillReads)
{
    // A daemon started before the upgrade, still running: the "already
    // running" guard and the stale-session checks must still see it.
    json resolved = legacyDefaults();
    resolved["modules"]["core_service"]["transports"].push_back(
        {{"protocol", "tcp_ssl"}, {"host", "0.0.0.0"}, {"port", 6443}, {"ca_file", "/ca.pem"}});
    resolved["ssl"] = {{"cert", "/etc/ssl/cert.pem"}, {"key", "/etc/ssl/key.pem"}, {"ca", ""}};
    resolved["insecure_tcp"] = true;
    fs::path p(DaemonRuntimeStateFile::filePath());
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc)
        << json{{"version", 2}, {"instance_id", "old456"}, {"pid", 4242},
                {"started_at", "2026-01-01T00:00:00Z"}, {"resolved", resolved}}.dump(4);

    DaemonRuntimeState got = DaemonRuntimeStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.pid, 4242);
    EXPECT_EQ(got.instanceId, "old456");
    EXPECT_EQ(got.resolved.modulesDirs, std::vector<std::string>{"/opt/modules"});
}

TEST_F(DaemonStateTest, RemovedKeys_ClientTransportsAreRefusedByName)
{
    const auto withEntry = [](json entry) {
        return json{{"version", 2}, {"token_file", "auto.json"},
                    {"daemon", {{"core_service", entry}}}};
    };
    EXPECT_TRUE(refusedAsRemoved(clientConfigError(withEntry({{"transport", "tcp"}})),
                                 "daemon.core_service.transport: tcp"));
    EXPECT_TRUE(refusedAsRemoved(clientConfigError(withEntry({{"transport", "tcp_ssl"}})),
                                 "daemon.core_service.transport: tcp_ssl"));
    for (const char* field : {"host", "port", "codec", "ca", "verify_peer"}) {
        EXPECT_TRUE(refusedAsRemoved(
            clientConfigError(withEntry({{"transport", "local"}, {field, "x"}})),
            std::string("daemon.core_service.") + field));
    }
    // The dial spec every daemon wrote is still a dial spec.
    EXPECT_EQ(clientConfigError(withEntry({{"transport", "local"}})), "");
}

TEST_F(DaemonStateTest, RemovedKeys_TheMessageNamesTheReplacements)
{
    const std::string message = removedWithTcpTransports("insecure_tcp");
    for (const char* part : {"`insecure_tcp`", "local socket only", "Remote Runtime Control",
                             "logosctl peer invite --runtime-control", "logosctl remote pair",
                             "--remote", "peer the two"})
        EXPECT_NE(message.find(part), std::string::npos) << part << " missing: " << message;
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
                           {"daemon", {{"core_service", {{"transport", 42}}}}}}),
        "daemon.core_service.transport"));
    EXPECT_TRUE(namesKey(
        clientConfigError({{"version", 2},
                           {"daemon", {{"core_service", {{"transport", "locall"}}}}}}),
        "daemon.core_service.transport"));
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
           "    transport: local\n";

    ClientState got = ClientStateFile::read();
    EXPECT_TRUE(got.fileOk);
    EXPECT_EQ(got.tokenFile, "auto.json");
    EXPECT_EQ(got.instanceId, "inst-A");
    EXPECT_EQ(got.daemon.count("core_service"), 1u);
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
    const json doc = {{"max_size_mb", 25}, {"console", true}, {"file", "daemon.log"}};
    auto back = yaml_json::parse(yaml_json::dump(doc));
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE((*back)["max_size_mb"].is_number_integer());
    EXPECT_EQ((*back)["max_size_mb"].get<int>(), 25);
    EXPECT_TRUE((*back)["console"].is_boolean());
    EXPECT_EQ((*back)["file"].get<std::string>(), "daemon.log");
}
