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
    fs::path logoscoreBinary;

    void SetUp() override {
        // Check for LOGOSCORE_BINARY environment variable first
        const char* envBinary = std::getenv("LOGOSCORE_BINARY");
        if (envBinary && fs::exists(envBinary)) {
            logoscoreBinary = envBinary;
            return;
        }

        // Get the directory where the test executable is located
        fs::path execDir = getExecutableDir();

        // Find the logoscore binary - try multiple locations
        std::vector<fs::path> searchPaths;

        // First, check in the same directory as the test executable (Nix builds)
        if (!execDir.empty()) {
            searchPaths.push_back(execDir / "logoscore");
        }

        // Then try paths relative to current working directory
        searchPaths.push_back(fs::current_path() / ".." / "bin" / "logoscore");
        searchPaths.push_back(fs::current_path() / "bin" / "logoscore");
        searchPaths.push_back(fs::current_path() / ".." / ".." / "bin" / "logoscore");
        searchPaths.push_back(fs::current_path().parent_path() / "logoscore");

        for (const auto& path : searchPaths) {
            if (fs::exists(path)) {
                logoscoreBinary = fs::canonical(path);
                return;
            }
        }

        // Binary not found, skip tests
        std::string triedPaths;
        for (size_t i = 0; i < searchPaths.size(); ++i) {
            if (i > 0) triedPaths += ", ";
            triedPaths += "\"" + searchPaths[i].string() + "\"";
        }
        GTEST_SKIP() << "logoscore binary not found. Set LOGOSCORE_BINARY env var or build the binary first. Tried: "
                     << triedPaths;
    }

    // Helper to run logoscore command
    int runLogoscore(const std::string& args, std::string* output = nullptr) {
        std::string cmd = logoscoreBinary.string() + " " + args;
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

    // Helper to run logoscore with timeout (for commands that run event loop)
    int runLogoscoreWithTimeout(const std::string& args, std::string* output, int timeoutSecs = 2) {
        std::string cmd = "timeout " + std::to_string(timeoutSecs) + " " + logoscoreBinary.string() + " " + args + " 2>&1";
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
    int exitCode = runLogoscore("--help", &output);

    EXPECT_EQ(exitCode, 0);
    // New help text includes subcommands
    EXPECT_NE(output.find("logoscore"), std::string::npos) << "Help should contain app name";
    EXPECT_NE(output.find("status"), std::string::npos) << "Help should list status command";
    EXPECT_NE(output.find("load-module"), std::string::npos) << "Help should list load-module command";
    EXPECT_NE(output.find("call"), std::string::npos) << "Help should list call command";
    EXPECT_NE(output.find("watch"), std::string::npos) << "Help should list watch command";
    EXPECT_NE(output.find("--json"), std::string::npos) << "Help should document --json flag";
}

TEST_F(CLITest, HelpShortFlag) {
    std::string output;
    int exitCode = runLogoscore("-h", &output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("logoscore"), std::string::npos);
}

TEST_F(CLITest, VersionCommand) {
    std::string output;
    int exitCode = runLogoscore("--version", &output);

    EXPECT_EQ(exitCode, 0);
    // The version string is build-derived (release version / pre-release sha /
    // "dev"), so assert on the stable tool-name prefix rather than a literal
    // version number.
    EXPECT_NE(output.find("logoscore version"), std::string::npos) << "Version output should identify logoscore";
}

TEST_F(CLITest, NoArgs_ShowsHelp) {
    std::string output;
    int exitCode = runLogoscore("", &output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("logoscore"), std::string::npos) << "No args should show help";
}

// ═════════════════════════════════════════════════════════════════════════════
// Client commands without daemon (should fail gracefully)
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, Status_NoDaemon) {
    std::string output;
    int exitCode = runLogoscore("status --json", &output);
    // Should report not_running (exit 1) or connection error (exit 2)
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, ListModules_NoDaemon) {
    std::string output;
    int exitCode = runLogoscore("list-modules --json", &output);
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, LoadModule_NoDaemon) {
    std::string output;
    int exitCode = runLogoscore("load-module waku --json", &output);
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, ModuleInfo_NoDaemon) {
    std::string output;
    int exitCode = runLogoscore("module-info chat --json", &output);
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, Stats_NoDaemon) {
    std::string output;
    int exitCode = runLogoscore("stats --json", &output);
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
    int exitCode = runLogoscore("status --json", &output);
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    EXPECT_LE(secs, 5) << "status should return within 5 seconds (took " << secs << "s). "
                        << "Likely an RPC timeout or token key misconfiguration.";
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, LoadModule_NoDaemon_ReturnsFast) {
    auto start = std::chrono::steady_clock::now();
    std::string output;
    int exitCode = runLogoscore("load-module test --json", &output);
    auto elapsed = std::chrono::steady_clock::now() - start;

    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    EXPECT_LE(secs, 5) << "load-module should return within 5 seconds (took " << secs << "s).";
    EXPECT_NE(exitCode, 0);
}

TEST_F(CLITest, Stop_NoDaemon_ReturnsFast) {
    auto start = std::chrono::steady_clock::now();
    std::string output;
    int exitCode = runLogoscore("stop --json", &output);
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
    int exitCode = runLogoscore("--help", &output);
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
    int exitCode = runLogoscoreWithTimeout("-D --modules-dir /tmp/x", &output, 5);
    EXPECT_EQ(exitCode, 109)
        << "A removed flag must be a parse error, not ignored (and must not "
           "start a daemon -- 124 would mean it did). Output:\n" << output;
}

TEST_F(CLITest, DaemonConfigSet_RejectsMalformedYaml) {
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logoscore_cli_badyaml_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "bad.yaml";
    { std::ofstream ofs(doc, std::ios::trunc); ofs << "modules:\n  - [unclosed\n"; }

    std::string output;
    int exitCode = runLogoscoreWithTimeout(
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
        ("logoscore_cli_badkey_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    const fs::path doc = cfgDir / "typo.yaml";
    { std::ofstream ofs(doc, std::ios::trunc); ofs << "insecureTcp: true\n"; }

    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("insecure_tcp"), std::string::npos)
        << "The error should name the correct spelling. Output:\n" << output;
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonConfigSet_RoundTripsThroughShow) {
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logoscore_cli_rt_" + std::to_string(::getpid()));
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
    int exitCode = runLogoscoreWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config set " + doc.string(),
        &output, 5);
    ASSERT_EQ(exitCode, 0) << "Output:\n" << output;

    std::string shown;
    exitCode = runLogoscoreWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config show --human", &shown, 5);
    EXPECT_EQ(exitCode, 0) << "Output:\n" << shown;
    EXPECT_NE(shown.find("8645"), std::string::npos) << "Output:\n" << shown;
    EXPECT_NE(shown.find("insecure_tcp"), std::string::npos) << "Output:\n" << shown;
    fs::remove_all(cfgDir);
}

TEST_F(CLITest, DaemonConfigShow_AbsentIsNotAnError) {
    // A session with no config runs on defaults; that is a normal state.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logoscore_cli_absent_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir);
    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--config-dir " + cfgDir.string() + " daemon config show --human", &output, 5);
    EXPECT_EQ(exitCode, 0) << "Output:\n" << output;
    fs::remove_all(cfgDir);
}
