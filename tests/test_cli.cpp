#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#endif

namespace fs = std::filesystem;

// Helper function to get the directory of the current executable
static fs::path getExecutableDir() {
#ifdef __APPLE__
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        return fs::path(path).parent_path();
    }
#elif defined(__linux__)
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        return fs::path(path).parent_path();
    }
#endif
    return fs::path();
}

class CLITest : public ::testing::Test {
protected:
    fs::path logosctlBinary;

    void SetUp() override {
        // Check for LOGOSCTL_BINARY environment variable first
        const char* envBinary = std::getenv("LOGOSCTL_BINARY");
        if (envBinary && fs::exists(envBinary)) {
            logosctlBinary = envBinary;
            return;
        }

        // Get the directory where the test executable is located
        fs::path execDir = getExecutableDir();

        // Find the logosctl binary - try multiple locations
        std::vector<fs::path> searchPaths;

        // First, check in the same directory as the test executable (Nix builds)
        if (!execDir.empty()) {
            searchPaths.push_back(execDir / "logosctl");
        }

        // Then try paths relative to current working directory
        searchPaths.push_back(fs::current_path() / ".." / "bin" / "logosctl");
        searchPaths.push_back(fs::current_path() / "bin" / "logosctl");
        searchPaths.push_back(fs::current_path() / ".." / ".." / "bin" / "logosctl");
        searchPaths.push_back(fs::current_path().parent_path() / "logosctl");

        for (const auto& path : searchPaths) {
            if (fs::exists(path)) {
                logosctlBinary = fs::canonical(path);
                return;
            }
        }

        // Binary not found, skip tests
        std::string triedPaths;
        for (size_t i = 0; i < searchPaths.size(); ++i) {
            if (i > 0) triedPaths += ", ";
            triedPaths += "\"" + searchPaths[i].string() + "\"";
        }
        GTEST_SKIP() << "logosctl binary not found. Set LOGOSCTL_BINARY env var or build the binary first. Tried: "
                     << triedPaths;
    }

    // Helper to run logosctl command
    int runLogosctl(const std::string& args, std::string* output = nullptr) {
        std::string cmd = logosctlBinary.string() + " " + args;
        if (output) {
            cmd += " 2>&1";
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) return -1;

            char buffer[128];
            while (fgets(buffer, sizeof(buffer), pipe)) {
                *output += buffer;
            }
            int status = pclose(pipe);
            return WEXITSTATUS(status);
        } else {
            int status = system(cmd.c_str());
            return WEXITSTATUS(status);
        }
    }

    // Helper to run logosctl with timeout (for commands that run event loop)
    int runLogosctlWithTimeout(const std::string& args, std::string* output, int timeoutSecs = 2) {
        std::string cmd = "timeout " + std::to_string(timeoutSecs) + " " + logosctlBinary.string() + " " + args + " 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return -1;

        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            *output += buffer;
        }
        int status = pclose(pipe);
        return WEXITSTATUS(status);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Help and version tests
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, HelpCommand) {
    std::string output;
    int exitCode = runLogosctl("--help", &output);

    EXPECT_EQ(exitCode, 0);
    // New help text includes subcommands
    EXPECT_NE(output.find("logosctl"), std::string::npos) << "Help should contain app name";
    EXPECT_NE(output.find("status"), std::string::npos) << "Help should list status command";
    // Help lists the groups, not the internal hyphenated dispatch tokens.
    EXPECT_NE(output.find("module"), std::string::npos) << "Help should list the module group";
    EXPECT_NE(output.find("package"), std::string::npos) << "Help should list the package group";
    EXPECT_NE(output.find("catalog"), std::string::npos) << "Help should list the catalog group";
    EXPECT_EQ(output.find("load-module"), std::string::npos)
        << "load-module is an internal dispatch token and must stay out of help";
    EXPECT_NE(output.find("call"), std::string::npos) << "Help should list call command";
    EXPECT_NE(output.find("watch"), std::string::npos) << "Help should list watch command";
    EXPECT_NE(output.find("--json"), std::string::npos) << "Help should document --json flag";
}

TEST_F(CLITest, HelpShortFlag) {
    std::string output;
    int exitCode = runLogosctl("-h", &output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("logosctl"), std::string::npos);
}

TEST_F(CLITest, VersionCommand) {
    std::string output;
    int exitCode = runLogosctl("--version", &output);

    EXPECT_EQ(exitCode, 0);
    // The version string is build-derived (release version / pre-release sha /
    // "dev"), so assert on the stable tool-name prefix rather than a literal
    // version number.
    EXPECT_NE(output.find("logosctl version"), std::string::npos) << "Version output should identify logosctl";
}

TEST_F(CLITest, NoArgs_ShowsHelp) {
    std::string output;
    int exitCode = runLogosctl("", &output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("logosctl"), std::string::npos) << "No args should show help";
}

// ═════════════════════════════════════════════════════════════════════════════
// Client commands without daemon (should fail gracefully)
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, Status_NoDaemon) {
    std::string output;
    int exitCode = runLogosctl("status --json", &output);
    // Should report not_running (exit 1) or connection error (exit 2)
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, ListModules_NoDaemon) {
    std::string output;
    int exitCode = runLogosctl("list-modules --json", &output);
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, LoadModule_NoDaemon) {
    std::string output;
    int exitCode = runLogosctl("load-module waku --json", &output);
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, ModuleInfo_NoDaemon) {
    std::string output;
    int exitCode = runLogosctl("module-info chat --json", &output);
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, Stats_NoDaemon) {
    std::string output;
    int exitCode = runLogosctl("stats --json", &output);
    EXPECT_NE(exitCode, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Timing tests — client commands must return quickly (catches RPC hangs)
// If the RPC layer has a misconfigured token key or missing timeout,
// commands hang for 20+ seconds waiting for capability_module negotiation.
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, Status_NoDaemon_ReturnsFast) {
    auto start = std::chrono::steady_clock::now();
    std::string output;
    int exitCode = runLogosctl("status --json", &output);
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    EXPECT_LE(secs, 5) << "status should return within 5 seconds (took " << secs << "s). "
                        << "Likely an RPC timeout or token key misconfiguration.";
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, LoadModule_NoDaemon_ReturnsFast) {
    auto start = std::chrono::steady_clock::now();
    std::string output;
    int exitCode = runLogosctl("load-module test --json", &output);
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    EXPECT_LE(secs, 5) << "load-module should return within 5 seconds (took " << secs << "s).";
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, Stop_NoDaemon_ReturnsFast) {
    auto start = std::chrono::steady_clock::now();
    std::string output;
    int exitCode = runLogosctl("stop --json", &output);
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    EXPECT_LE(secs, 5) << "stop should return within 5 seconds (took " << secs << "s).";
    EXPECT_NE(exitCode, 0);
}
// ═════════════════════════════════════════════════════════════════════════════
// Configuration moved from flags to the session's YAML documents.
//
// The old per-flag surface (-m/--modules-dir, --persistence-path,
// --module-transport, --insecure-tcp, --access-policy, --access-group,
// --persist-config, and the seven --client-* dial flags) is gone. What those
// flags used to validate is now validated when the document is installed, so
// these tests moved with it: a bad value is rejected by `daemon config set` /
// `client config set` rather than at parse time.
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, Help_ShowsOnlyTheSurvivingGlobalFlags) {
    std::string output;
    int exitCode = runLogosctl("--help", &output);
    EXPECT_EQ(exitCode, 0);

    // --config-dir survives because it selects *which* session to act on, so
    // it cannot itself live inside one.
    EXPECT_NE(output.find("--config-dir"), std::string::npos)
        << "Output:\n" << output;

    for (const char* gone : {"--modules-dir", "--persistence-path",
                             "--module-transport", "--insecure-tcp",
                             "--access-policy", "--access-group",
                             "--persist-config", "--client-transport",
                             "--client-codec", "--token-file", "--ssl-ca"}) {
        EXPECT_EQ(output.find(gone), std::string::npos)
            << gone << " should no longer exist as a flag. Output:\n" << output;
    }
}

TEST_F(CLITest, RemovedFlags_AreRejectedNotIgnored) {
    // Silently accepting a flag that no longer does anything would leave the
    // operator's intent unapplied with nothing to explain it.
    std::string output;
    int exitCode = runLogosctlWithTimeout("-D --modules-dir /tmp/x", &output, 5);
    EXPECT_EQ(exitCode, 109)
        << "A removed flag must be a parse error, not ignored (and must not "
           "start a daemon -- 124 would mean it did). Output:\n" << output;
}

TEST_F(CLITest, DaemonConfigSet_RejectsMalformedYaml) {
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logosctl_cli_badyaml_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "bad.yaml";
    { std::ofstream ofs(doc, std::ios::trunc); ofs << "modules:\n  - [unclosed\n"; }

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    // The existing config must survive a rejected document.
    EXPECT_FALSE(fs::exists(cfgDir / "daemon" / "config.yaml"))
        << "A malformed document must not be written.";
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonConfigSet_RejectsUnknownKeys) {
    // `insecureTcp` is a near-miss for `insecure_tcp`. The loader ignores
    // unrecognised keys, so without this check the daemon would boot with the
    // operator's intent silently dropped.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logosctl_cli_badkey_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "typo.yaml";
    { std::ofstream ofs(doc, std::ios::trunc); ofs << "insecureTcp: true\n"; }

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("insecure_tcp"), std::string::npos)
        << "The error should name the correct spelling. Output:\n" << output;
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonConfigSet_RoundTripsThroughShow) {
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logosctl_cli_rt_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "node.yaml";
    {
        std::ofstream ofs(doc, std::ios::trunc);
        ofs << "insecure_tcp: true\n"
               "modules:\n"
               "  core_service:\n"
               "    - protocol: tcp\n"
               "      host: 127.0.0.1\n"
               "      port: 8645\n";
    }

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5);
    ASSERT_EQ(exitCode, 0) << "Output:\n" << output;

    std::string shown;
    exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config show --human", &shown, 5);
    EXPECT_EQ(exitCode, 0) << "Output:\n" << shown;
    EXPECT_NE(shown.find("8645"), std::string::npos) << "Output:\n" << shown;
    EXPECT_NE(shown.find("insecure_tcp"), std::string::npos) << "Output:\n" << shown;
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonConfigSet_AcceptsEverySignaturePolicyValue) {
    for (const char* policy : {"none", "warn", "require"}) {
        const fs::path cfgDir = fs::temp_directory_path() /
            ("logosctl_cli_sigok_" + std::string(policy) + "_" +
             std::to_string(::getpid()));
        fs::remove_all(cfgDir);
        fs::create_directories(cfgDir);
        const fs::path doc = cfgDir / "node.yaml";
        { std::ofstream ofs(doc, std::ios::trunc);
          ofs << "signature_policy: " << policy << "\n"; }

        std::string output;
        int exitCode = runLogosctlWithTimeout(
            "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
            &output, 5);
        EXPECT_EQ(exitCode, 0) << policy << " Output:\n" << output;

        std::string shown;
        exitCode = runLogosctlWithTimeout(
            "--config-dir " + cfgDir.string() + " daemon config show --human",
            &shown, 5);
        EXPECT_EQ(exitCode, 0) << "Output:\n" << shown;
        EXPECT_NE(shown.find(policy), std::string::npos) << "Output:\n" << shown;
        fs::remove_all(cfgDir);
    }
}

TEST_F(CLITest, DaemonConfigSet_RejectsUnknownSignaturePolicy) {
    // package_manager ignores a policy it does not recognise, so `required`
    // (the key takes `require`) would leave it on the default `warn` while
    // `daemon config show` kept reporting the operator's stricter intent.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logosctl_cli_sigbad_" + std::to_string(::getpid()));
    fs::remove_all(cfgDir);
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "node.yaml";
    { std::ofstream ofs(doc, std::ios::trunc); ofs << "signature_policy: required\n"; }

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("require"), std::string::npos)
        << "The error should name the accepted values. Output:\n" << output;
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonStart_RefusesTlsListenerWithNoCertificate) {
    // A tcp_ssl listener with no material binds fine and then fails every
    // handshake with "no shared cipher", which reads like a client fault.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logosctl_cli_nocert_" + std::to_string(::getpid()));
    fs::remove_all(cfgDir);
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "node.yaml";
    {
        std::ofstream ofs(doc, std::ios::trunc);
        ofs << "modules:\n"
               "  core_service:\n"
               "    - protocol: tcp_ssl\n"
               "      host: 127.0.0.1\n"
               "      port: 8645\n";
    }
    std::string output;
    ASSERT_EQ(runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5), 0) << "Output:\n" << output;

    output.clear();
    // 124 would mean a daemon actually started on a certificate-less listener.
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " -D", &output, 15);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("core_service"), std::string::npos)
        << "The error should name the listener. Output:\n" << output;
    EXPECT_NE(output.find("ssl:"), std::string::npos)
        << "The error should name the top-level block as one of the two places "
           "the material can come from. Output:\n" << output;
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonConfigShow_AbsentIsNotAnError) {
    // A session with no config runs on defaults; that is a normal state.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logosctl_cli_absent_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config show --human", &output, 5);
    EXPECT_EQ(exitCode, 0) << "Output:\n" << output;
    fs::remove_all(cfgDir);
}

// ═════════════════════════════════════════════════════════════════════════════
// A malformed document must be an error, never a crash, and never a write.
//
// The reader used nlohmann's `json::value(key, default)`, which throws when the
// key is present with a different type than the default. Nothing caught it, so
// `modules_dirs: /single/path` — a scalar where a list belongs — terminated the
// binary:
//
//   libc++abi: terminating due to uncaught exception of type
//   nlohmann::detail::type_error: [json.exception.type_error.302]
//   type must be array, but is string
//
// A process killed by SIGABRT surfaces here as exit 134, so asserting exit 1
// is what distinguishes "reported it" from "died on it".
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// Per-test config dir plus a document to feed `config set`. Named after the
// test so parallel cases never share one.
struct ConfigFixture {
    fs::path dir;
    fs::path doc;

    ConfigFixture(const std::string& name, const std::string& body)
    {
        dir = fs::temp_directory_path() /
              ("logosctl_cli_" + name + "_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        doc = dir / "doc.yaml";
        std::ofstream ofs(doc, std::ios::trunc);
        ofs << body;
    }
    ~ConfigFixture() { fs::remove_all(dir); }

    fs::path daemonConfig() const { return dir / "daemon" / "config.yaml"; }
    fs::path clientConfig() const { return dir / "client" / "config.yaml"; }
};

} // namespace

TEST_F(CLITest, DaemonConfigSet_TypeMismatchIsReportedNotFatal) {
    ConfigFixture fx("typemismatch", "modules_dirs: /single/path\n");

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " daemon config set " + fx.doc.string(),
        &output, 5);

    EXPECT_EQ(exitCode, 1)
        << "A mistyped value must be reported (exit 1), not abort the process "
           "(exit 134). Output:\n" << output;
    EXPECT_NE(output.find("modules_dirs"), std::string::npos)
        << "The error must name the offending key. Output:\n" << output;
    EXPECT_EQ(output.find("terminating due to uncaught exception"), std::string::npos)
        << "Output:\n" << output;
    EXPECT_FALSE(fs::exists(fx.daemonConfig()))
        << "A document that cannot be understood must not be written.";
}

TEST_F(CLITest, DaemonConfigSet_SchemaInvalidDocumentIsNotWritten) {
    // This document passes the YAML parse and the key allowlist, and fails
    // only the schema. Validation used to run AFTER the write, so the command
    // exited 1 having already installed a config the daemon would refuse to
    // boot from.
    ConfigFixture fx("schemainvalid",
                     "modules:\n"
                     "  core_service:\n"
                     "    - protocol: tcpp\n"
                     "      host: 127.0.0.1\n"
                     "      port: 8645\n");

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " daemon config set " + fx.doc.string(),
        &output, 5);

    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_FALSE(fs::exists(fx.daemonConfig()))
        << "A rejected document must not be left on disk — the session would "
           "be holding a config the daemon cannot boot from.";
    EXPECT_NE(output.find("protocol"), std::string::npos)
        << "The error must name the offending key. Output:\n" << output;
}

TEST_F(CLITest, DaemonConfigSet_RejectionLeavesThePreviousConfigIntact) {
    ConfigFixture fx("preserve", "insecure_tcp: true\n");

    std::string output;
    ASSERT_EQ(runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " daemon config set " + fx.doc.string(),
        &output, 5), 0) << "Output:\n" << output;
    ASSERT_TRUE(fs::exists(fx.daemonConfig()));

    const fs::path bad = fx.dir / "bad.yaml";
    { std::ofstream ofs(bad, std::ios::trunc); ofs << "modules_dirs: /single/path\n"; }
    output.clear();
    EXPECT_EQ(runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " daemon config set " + bad.string(),
        &output, 5), 1) << "Output:\n" << output;

    std::ifstream ifs(fx.daemonConfig());
    const std::string body((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("insecure_tcp"), std::string::npos)
        << "The accepted config must survive a rejected one. It now reads:\n" << body;
    EXPECT_EQ(body.find("modules_dirs"), std::string::npos)
        << "The rejected document must not have been applied. It now reads:\n" << body;
}

TEST_F(CLITest, ClientConfigSet_TypeMismatchIsReportedNotFatal) {
    // The client half of the same hazard, and the same guarantee.
    ConfigFixture fx("clienttype",
                     "token_file: auto.json\n"
                     "daemon:\n"
                     "  core_service:\n"
                     "    transport: tcp\n"
                     "    host: 127.0.0.1\n"
                     "    port: \"6001\"\n");

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " client config set " + fx.doc.string(),
        &output, 5);

    EXPECT_EQ(exitCode, 1)
        << "A mistyped value must be reported (exit 1), not abort the process "
           "(exit 134). Output:\n" << output;
    EXPECT_NE(output.find("port"), std::string::npos)
        << "The error must name the offending key. Output:\n" << output;
    EXPECT_FALSE(fs::exists(fx.clientConfig()))
        << "A document that cannot be understood must not be written.";
}
