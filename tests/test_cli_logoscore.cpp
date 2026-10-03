// ─────────────────────────────────────────────────────────────────────────────
// logoscore's copy of this suite.
//
// While both binaries ship, both get tested. This file is a duplicate of the
// logosctl suite frozen against logoscore's surface -- the old flags, the flat
// subcommands, ~/.logoscore -- so a change that quietly altered the tool people
// actually use fails here rather than in someone's deployment.
//
// Deliberately a copy rather than a parameterised shared suite: the two
// surfaces genuinely differ, and this whole file gets deleted when logoscore
// is retired. Keeping them separate makes that a delete instead of an unpick.
// ─────────────────────────────────────────────────────────────────────────────

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
// Relative path resolution — logos_core cannot load plugin metadata from
// relative paths (dlopen fails to resolve RPATH).  logoscore must resolve
// --modules-dir to an absolute path before calling logos_core_add_modules_dir.
//
// These tests verify this by checking that the "Added plugins directory:"
// debug message contains an absolute path, even when the CLI receives a
// relative one.
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, DaemonMode_RelativePath_ResolvedToAbsolute) {
    fs::path parentDir = fs::temp_directory_path() / "logoscore_test_relpath_daemon";
    fs::path modulesDir = parentDir / "my_modules";
    fs::create_directories(modulesDir);

    // cd into parentDir, start daemon with relative "./my_modules"
    std::string cmd = "cd " + parentDir.string() + " && HOME=" + parentDir.string()
        + " timeout 5 "
        + logoscoreBinary.string()
        + " -D --verbose --modules-dir ./my_modules 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        output += buffer;
    pclose(pipe);

    std::string marker = "Added plugins directory:";
    auto pos = output.find(marker);
    ASSERT_NE(pos, std::string::npos) << "Should see plugins directory message. Output:\n" << output;

    std::string afterMarker = output.substr(pos + marker.size());
    bool isAbsolute = afterMarker.find("/my_modules") != std::string::npos;
    bool isRelative = afterMarker.find("\"./my_modules\"") != std::string::npos;
    EXPECT_TRUE(isAbsolute && !isRelative)
        << "Daemon relative --modules-dir should be resolved to absolute before passing to logos_core. "
        << "Output:\n" << output;

    fs::remove_all(parentDir);
}

// ═════════════════════════════════════════════════════════════════════════════
// Persistence path option
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, HelpCommand_ShowsPersistencePath) {
    std::string output;
    int exitCode = runLogoscore("--help", &output);
    EXPECT_EQ(exitCode, 0);
    EXPECT_NE(output.find("--persistence-path"), std::string::npos)
        << "Help should document --persistence-path option. Output:\n" << output;
}

// NOTE: module manifest discovery (with/without "type") was previously
// exercised through inline mode; that behaviour lives in liblogos and is
// covered there. The daemon path is verified by the integration tests.

// ═════════════════════════════════════════════════════════════════════════════
// Inline mode removed — daemon-only flags must be rejected, not silently ignored
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, DaemonFlags_NoDaemonNoSubcommand_Rejected) {
    // `logoscore -m <dir>` with no -D and no subcommand used to start inline
    // mode; it must now fail with guidance toward the daemon/client workflow.
    std::string output;
    int exitCode = runLogoscore("--modules-dir /tmp/logoscore_test_x", &output);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("daemon"), std::string::npos)
        << "Should point at the daemon (-D) workflow. Output:\n" << output;
}

TEST_F(CLITest, DaemonFlags_WithClientSubcommand_Rejected) {
    // Daemon-only flags alongside a client subcommand are a no-op trap; reject
    // them before attempting the command, regardless of whether a daemon runs.
    std::string output;
    int exitCode = runLogoscore("--modules-dir /tmp/logoscore_test_x status", &output);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("daemon"), std::string::npos)
        << "Should reject -m with a client subcommand. Output:\n" << output;
}

// ═════════════════════════════════════════════════════════════════════════════
// The tcp and tcp_ssl transports are gone. The flags that configured them, and
// their environment variables, are refused with one message rather than
// ignored: a daemon or client that meant to use them must not quietly run
// local-only. The timeout helper turns a daemon that started anyway into 124.
// ═════════════════════════════════════════════════════════════════════════════

static void expectRemovedWithTcp(const std::string& output, const std::string& key)
{
    EXPECT_NE(output.find("`" + key + "` was removed with the tcp and tcp_ssl transports"),
              std::string::npos) << "Output:\n" << output;
    EXPECT_NE(output.find("logosctl remote pair"), std::string::npos)
        << "The refusal should point at Remote Runtime Control. Output:\n" << output;
}

TEST_F(CLITest, ModuleTransport_TcpListenerIsRefused) {
    for (const std::string spec : {"core_service=tcp,host=0.0.0.0,port=6000",
                                   "core_service=tcp_ssl,cert=c.pem,key=k.pem",
                                   "core_service=local,port=6000"}) {
        std::string output;
        const int exitCode = runLogoscoreWithTimeout("-D --module-transport " + spec, &output, 5);
        EXPECT_EQ(exitCode, 1) << spec << " Output:\n" << output;
        expectRemovedWithTcp(output, "--module-transport " + spec);
    }
}

TEST_F(CLITest, RemovedFlags_AreRefusedNotIgnored) {
    for (const std::string flag : {"--insecure-tcp", "--no-verify-peer",
                                   "--client-transport tcp", "--client-tcp-host h",
                                   "--client-tcp-port 6000", "--client-codec json",
                                   "--ssl-ca ca.pem"}) {
        std::string output;
        const int exitCode = runLogoscoreWithTimeout(flag + " status", &output, 5);
        EXPECT_EQ(exitCode, 1) << flag << " Output:\n" << output;
        expectRemovedWithTcp(output, flag.substr(0, flag.find(' ')));
    }
}

TEST_F(CLITest, RemovedEnvVars_AreRefusedByClientCommands) {
    ::setenv("LOGOSCORE_CLIENT_TRANSPORT", "tcp_ssl", 1);
    std::string output;
    const int exitCode = runLogoscoreWithTimeout("status", &output, 5);
    ::unsetenv("LOGOSCORE_CLIENT_TRANSPORT");
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    expectRemovedWithTcp(output, "$LOGOSCORE_CLIENT_TRANSPORT");
}

TEST_F(CLITest, PersistedConfigWithRemovedKeys_DaemonRefusesToStart) {
    // A config.json --persist-config wrote with a plaintext listener enabled.
    // NAME=local is still accepted: the refusal comes from the config.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logoscore_cli_legacycfg_" + std::to_string(::getpid()));
    fs::create_directories(cfgDir / "daemon");
    std::ofstream(cfgDir / "daemon" / "config.json")
        << R"({"version":2,"insecure_tcp":true})" << "\n";

    std::string output;
    const int exitCode = runLogoscoreWithTimeout(
        "--config-dir " + cfgDir.string() + " -D --module-transport core_service=local",
        &output, 10);
    fs::remove_all(cfgDir);
    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    expectRemovedWithTcp(output, "insecure_tcp");
    EXPECT_NE(output.find("does not start on a config it cannot load"), std::string::npos)
        << "Output:\n" << output;
}

// ═════════════════════════════════════════════════════════════════════════════
// BUG-028: --token-file must reject a file that exists but carries no usable
// token (missing/empty token field, or unparseable JSON), instead of marking
// the config usable and failing later at connect time.
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, TokenFile_PresentButEmptyTokenRejected) {
    // Stage a client/ dir with a token file that has no usable token field.
    const fs::path cfgDir = fs::temp_directory_path() /
        ("logoscore_cli_tf_" + std::to_string(::getpid()));
    const fs::path clientDir = cfgDir / "client";
    fs::create_directories(clientDir);
    {
        std::ofstream ofs(clientDir / "empty.json", std::ios::trunc);
        ofs << "{}\n";  // valid JSON, but no "token" key
    }
    // --token-file is a global client flag too; place it before `status`.
    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--config-dir " + cfgDir.string() +
        " --token-file empty.json status", &output, 5);
    fs::remove_all(cfgDir);
    EXPECT_EQ(exitCode, 1)
        << "A token file with no usable token must be rejected up front "
           "(exit 1), not accepted and failed later. Output:\n" << output;
    EXPECT_NE(output.find("token"), std::string::npos)
        << "Output:\n" << output;
}
