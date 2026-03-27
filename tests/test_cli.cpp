#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
    EXPECT_NE(output.find("1.0"), std::string::npos) << "Version output should contain version number";
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
// Inline mode (legacy flags) — backward compatibility
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, InlineMode_ModulesDirOption) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--verbose --modules-dir /tmp/test_modules", &output);

    EXPECT_NE(output.find("Added plugins directory:"), std::string::npos)
        << "Should see debug message that custom directory was added";
    EXPECT_NE(output.find("/tmp/test_modules"), std::string::npos)
        << "Should see the custom directory path in output";
}

TEST_F(CLITest, InlineMode_LoadModulesOption) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--verbose --load-modules fake_module_xyz", &output);

    EXPECT_NE(output.find("Module not found in known plugins:"), std::string::npos)
        << "Should see warning that module was not found";
    EXPECT_NE(output.find("fake_module_xyz"), std::string::npos)
        << "Should see the module name in output";
}

TEST_F(CLITest, InlineMode_ShortAliases) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--verbose -m /tmp/test_modules_alias", &output);

    EXPECT_NE(output.find("Added plugins directory:"), std::string::npos)
        << "Short alias -m should work";
    EXPECT_NE(output.find("/tmp/test_modules_alias"), std::string::npos);
}

TEST_F(CLITest, InlineMode_CallSyntax) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--verbose --call \"fake_module.someMethod()\"", &output);

    // Should try to execute the call and fail (module not loaded)
    EXPECT_NE(output.find("Plugin not loaded") + output.find("fake_module"), std::string::npos);
}

TEST_F(CLITest, InlineMode_CallShortAlias) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--verbose -c \"fake_module.testMethod()\"", &output);

    EXPECT_NE(output.find("Plugin not loaded") + output.find("fake_module"), std::string::npos);
}

TEST_F(CLITest, InlineMode_InvalidCallSyntax) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout("--verbose --call \"modulemethodname()\"", &output);

    EXPECT_NE(output.find("Invalid call syntax") + output.find("Skipping invalid call"), std::string::npos);
}

TEST_F(CLITest, InlineMode_MultipleCalls) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--verbose --call \"module1.method1()\" --call \"module2.method2()\"", &output);

    EXPECT_NE(output.find("Executing call") + output.find("module1"), std::string::npos);
}

TEST_F(CLITest, InlineMode_FileParameterSyntax) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--verbose --call \"fake_module.init(@/nonexistent/file.txt)\"", &output);

    EXPECT_NE(output.find("Failed to open file") + output.find("Plugin not loaded"), std::string::npos);
}

TEST_F(CLITest, InlineMode_ParameterParsing) {
    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--verbose --call \"fake_module.method('string param', 42, true)\"", &output);

    EXPECT_NE(output.find("3 params") + output.find("params"), std::string::npos);
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
// Module manifest discovery — manifest.json must have "type": "core"
// PackageManagerLib::getInstalledModules() filters by type and silently
// skips modules without it.
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, InlineMode_ManifestWithType_Discovered) {
    fs::path tmpDir = fs::temp_directory_path() / "logoscore_test_discovery";
    fs::path modDir = tmpDir / "test_discovery_module";
    fs::create_directories(modDir);

#if defined(__APPLE__)
    #if defined(__aarch64__)
        std::string variant = "darwin-arm64-dev";
    #else
        std::string variant = "darwin-x86_64-dev";
    #endif
#elif defined(__aarch64__)
    std::string variant = "linux-arm64-dev";
#elif defined(__x86_64__)
    std::string variant = "linux-x86_64-dev";
#else
    std::string variant = "linux-x86-dev";
#endif

    // Manifest WITH "type": "core" — should be discoverable
    {
        std::ofstream f(modDir / "manifest.json");
        f << R"({"name":"test_discovery_module","version":"1.0.0","type":"core","main":{")"
          << variant << R"(":"test_discovery_module_plugin.so"}})";
    }
    // Dummy plugin file so mainFilePath resolves
    { std::ofstream f(modDir / "test_discovery_module_plugin.so"); f << "dummy"; }

    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--verbose --modules-dir " + tmpDir.string() + " --quit-on-finish", &output, 5);

    // Module should appear in discovery output (processPlugin may fail on the
    // dummy binary, but the important thing is PackageManagerLib found it)
    bool foundInOutput = output.find("test_discovery_module") != std::string::npos;
    EXPECT_TRUE(foundInOutput)
        << "Module with 'type: core' in manifest should be discovered. Output:\n" << output;

    fs::remove_all(tmpDir);
}

// ═════════════════════════════════════════════════════════════════════════════
// Relative path resolution — logos_core cannot load plugin metadata from
// relative paths (dlopen fails to resolve RPATH).  logoscore must resolve
// --modules-dir to an absolute path before calling logos_core_add_plugins_dir.
//
// These tests verify this by checking that the "Added plugins directory:"
// debug message contains an absolute path, even when the CLI receives a
// relative one.
// ═════════════════════════════════════════════════════════════════════════════

TEST_F(CLITest, InlineMode_RelativePath_ResolvedToAbsolute) {
    fs::path parentDir = fs::temp_directory_path() / "logoscore_test_relpath";
    fs::path modulesDir = parentDir / "my_modules";
    fs::create_directories(modulesDir);

    // cd into parentDir, pass relative "./my_modules"
    std::string cmd = "cd " + parentDir.string() + " && timeout 5 "
        + logoscoreBinary.string()
        + " --verbose --modules-dir ./my_modules --quit-on-finish 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    std::string output;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe))
        output += buffer;
    pclose(pipe);

    // The "Added plugins directory:" message should contain an absolute path
    // (starts with '/'), NOT the relative "./my_modules"
    std::string marker = "Added plugins directory:";
    auto pos = output.find(marker);
    ASSERT_NE(pos, std::string::npos) << "Should see plugins directory message. Output:\n" << output;

    std::string afterMarker = output.substr(pos + marker.size());
    // The path in the message must be absolute (contain '/' followed by the dir name)
    bool isAbsolute = afterMarker.find("/my_modules") != std::string::npos;
    bool isRelative = afterMarker.find("\"./my_modules\"") != std::string::npos;
    EXPECT_TRUE(isAbsolute && !isRelative)
        << "Relative --modules-dir should be resolved to absolute before passing to logos_core. "
        << "Output:\n" << output;

    fs::remove_all(parentDir);
}

TEST_F(CLITest, DaemonMode_RelativePath_ResolvedToAbsolute) {
    fs::path parentDir = fs::temp_directory_path() / "logoscore_test_relpath_daemon";
    fs::path modulesDir = parentDir / "my_modules";
    fs::create_directories(modulesDir);

    // cd into parentDir, start daemon with relative "./my_modules"
    std::string cmd = "cd " + parentDir.string() + " && timeout 5 "
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

TEST_F(CLITest, InlineMode_ManifestWithoutType_NotDiscovered) {
    fs::path tmpDir = fs::temp_directory_path() / "logoscore_test_no_type";
    fs::path modDir = tmpDir / "test_notype_module";
    fs::create_directories(modDir);

#if defined(__APPLE__)
    #if defined(__aarch64__)
        std::string variant = "darwin-arm64-dev";
    #else
        std::string variant = "darwin-x86_64-dev";
    #endif
#elif defined(__aarch64__)
    std::string variant = "linux-arm64-dev";
#elif defined(__x86_64__)
    std::string variant = "linux-x86_64-dev";
#else
    std::string variant = "linux-x86-dev";
#endif

    // Manifest WITHOUT "type" field — should be silently skipped
    {
        std::ofstream f(modDir / "manifest.json");
        f << R"({"name":"test_notype_module","version":"1.0.0","main":{")"
          << variant << R"(":"test_notype_module_plugin.so"}})";
    }
    { std::ofstream f(modDir / "test_notype_module_plugin.so"); f << "dummy"; }

    std::string output;
    int exitCode = runLogoscoreWithTimeout(
        "--verbose --modules-dir " + tmpDir.string() + " --quit-on-finish", &output, 5);

    // Module should NOT be found because it lacks "type": "core"
    bool discoveredMsg = output.find("Discovered module:") != std::string::npos &&
                         output.find("test_notype_module") != std::string::npos;
    EXPECT_FALSE(discoveredMsg)
        << "Module without 'type: core' should NOT be discovered. Output:\n" << output;

    fs::remove_all(tmpDir);
}
