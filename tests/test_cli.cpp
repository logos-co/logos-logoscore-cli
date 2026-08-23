#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
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
// A session whose daemon is gone
//
// The cases above run against a machine with no session at all, where the
// client gives up before it dials and any command looks fast. The slow case is
// the one an operator actually hits: a session directory left behind by a
// daemon that is no longer there. The dial spec is still present and still
// "works" -- a LocalSocket client connects to a path with no listener without
// complaint -- so the request goes out, nothing answers, QtRO reports nothing,
// and the command waits out Timeout(20000) (logos-protocol, cpp/logos_mode.h).
//
// It comes in two shapes, and they need different evidence:
//
//   Crashed        daemon/state.json is still there, naming a dead pid.
//                  Caught by detectStaleSession() in Command::ensureConnected.
//
//   CleanlyStopped daemon/state.json was REMOVED and the socket unlinked, but
//                  client/config.yaml and the token remain. There is no pid to
//                  check, so the pid guard has nothing to say; what settles it
//                  is that the socket the dial resolves to is not there.
//
//   SocketLeftOver the socket file is still on disk with nobody behind it. A
//                  hard-killed daemon leaves this, and so does a clean stop
//                  for the window between its shutdown reply and QLocalServer's
//                  destructor -- which is exactly when the next command gets
//                  typed. Presence of the file settles nothing; being REFUSED
//                  by it does.
//
// Both must fail at once. These are end-to-end on purpose: the unit tests pin
// the logic against mocks, but only a real binary against a real session
// directory can show that the twenty seconds are actually gone.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// Beyond every platform's pid ceiling (macOS 99998, Linux's default 4194304),
// so it cannot collide with some unrelated process that happens to be running.
constexpr const char* kGonePid = "2147483646";

// An instance id no live daemon on this machine can be using, which is what
// makes the socket check meaningful without having to control $TMPDIR: the
// endpoint `logos_core_service_<this>` cannot exist.
constexpr const char* kDeadInstance = "deadbeef1234";

// Everything a client needs in order to try, and nothing at the other end.
struct DeadSessionFixture {
    enum Shape { Crashed, CleanlyStopped, SocketLeftOver };

    fs::path dir;
    // Non-empty only for SocketLeftOver: a private, SHORT $TMPDIR to hold the
    // dead socket. Short because sockaddr_un::sun_path is 104 bytes on macOS
    // and the platform temp dir alone eats half of that. Both the daemon and
    // the client resolve a bare socket name against $TMPDIR, so handing the
    // command this one is what puts the file where its dial will look.
    fs::path socketDir;
    bool     socketStaged = false;

    DeadSessionFixture(const std::string& name, Shape shape)
    {
        dir = fs::temp_directory_path() /
              ("logosctl_cli_" + name + "_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir / "client");
        fs::create_directories(dir / "daemon");

        // JSON, which is valid YAML, so the same text serves both readers.
        put(dir / "client" / "config.yaml",
            std::string(R"({"version":2,"token_file":"auto.json","instance_id":")")
                + kDeadInstance
                + R"(","daemon":{"core_service":{"transport":"local"}}})");
        put(dir / "client" / "auto.json", R"({"token":"0123456789abcdef"})");

        // A clean shutdown removes this file. A crash leaves it behind.
        if (shape == Crashed)
            put(dir / "daemon" / "state.json",
                std::string(R"({"version":2,"instance_id":")") + kDeadInstance
                    + R"(","pid":)" + kGonePid
                    + R"(,"started_at":"2026-01-01T00:00:00Z"})");

        if (shape == SocketLeftOver) {
            socketDir = fs::path("/tmp") / ("lgx" + std::to_string(::getpid()));
            fs::remove_all(socketDir);
            fs::create_directories(socketDir);
            socketStaged = bindThenAbandon(
                socketDir / (std::string("logos_core_service_") + kDeadInstance));
        }
    }
    ~DeadSessionFixture()
    {
        fs::remove_all(dir);
        if (!socketDir.empty()) fs::remove_all(socketDir);
    }

    // What the command line needs in front of it to make this session's socket
    // the one the dial resolves to. Empty for the shapes that do not stage one.
    std::string env() const
    {
        return socketDir.empty() ? std::string{}
                                 : "TMPDIR='" + socketDir.string() + "' ";
    }

    // Bind and listen, then close the listener WITHOUT unlinking -- leaving an
    // inode that exists and refuses. False if the path would not fit in
    // sun_path, which the caller reports as a skip rather than a failure.
    static bool bindThenAbandon(const fs::path& path)
    {
        const std::string p = path.string();
        sockaddr_un addr{};
        if (p.size() >= sizeof(addr.sun_path)) return false;
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, p.c_str(), p.size());

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        ::unlink(p.c_str());
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
            || ::listen(fd, 4) != 0) {
            ::close(fd);
            return false;
        }
        ::close(fd);          // listener gone; the inode stays
        return true;
    }

    static void put(const fs::path& path, const std::string& body)
    {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << body << "\n";
    }
};

// Every command whose first act is an RPC, in the spelling a person types.
const std::vector<std::string>& rpcCommands()
{
    static const std::vector<std::string> kCommands{
        "module ls",
        "module load chat",
        "module show chat",
        "module reload chat",
        "module unload chat",
        "call chat send hi",
        "stats",
        "stop",
        "watch chat",
        "package ls",
        "package show chat",
        "package deps chat",
        "package search chat",
        "package download chat",
        "package install chat",
        "catalog ls",
        "key ls",
    };
    return kCommands;
}

} // namespace

// The shared body. `run` is bound by the test to the fixture's own helper
// (which is protected, so a free function cannot reach it). `expectPid` is
// false for the cleanly-stopped shape, where there is no pid on disk to name
// -- the message points at the missing socket instead.
using LogosctlRunner = std::function<int(const std::string&, std::string*)>;
static void expectEveryRpcCommandFailsAtOnce(const LogosctlRunner& run,
                                             const DeadSessionFixture& fx,
                                             bool expectPid);

TEST_F(CLITest, CrashedSession_EveryRpcCommandFailsAtOnce) {
    DeadSessionFixture fx("crashedsession", DeadSessionFixture::Crashed);
    expectEveryRpcCommandFailsAtOnce(
        [this](const std::string& a, std::string* o) {
            return runLogosctlWithTimeout(a, o, 5);
        },
        fx, /*expectPid=*/true);
}

// The tidier way to end up here, and the one the pid guard cannot see: a
// normal `daemon stop` takes daemon/state.json and the socket with it and
// leaves the dial spec behind.
TEST_F(CLITest, CleanlyStoppedSession_EveryRpcCommandFailsAtOnce) {
    DeadSessionFixture fx("cleanstop", DeadSessionFixture::CleanlyStopped);
    expectEveryRpcCommandFailsAtOnce(
        [this](const std::string& a, std::string* o) {
            return runLogosctlWithTimeout(a, o, 5);
        },
        fx, /*expectPid=*/false);
}

// The window a stat cannot see through: the socket file is still on disk, so
// "is it there?" says yes, and only being REFUSED by it settles the question.
TEST_F(CLITest, SocketLeftOverWithNoListener_EveryRpcCommandFailsAtOnce) {
    DeadSessionFixture fx("socketleft", DeadSessionFixture::SocketLeftOver);
    if (!fx.socketStaged)
        GTEST_SKIP() << "could not bind a short-enough socket path under "
                     << fx.socketDir;

    // $TMPDIR is how the staged socket becomes the one the dial resolves to;
    // the fixture's env() carries it, so this runner cannot go through
    // runLogosctlWithTimeout (which has nowhere to put it).
    const fs::path bin = logosctlBinary;
    const std::string env = fx.env();
    expectEveryRpcCommandFailsAtOnce(
        [&bin, &env](const std::string& a, std::string* o) -> int {
            const std::string cmd = env + "timeout 5 " + bin.string() + " " + a + " 2>&1";
            FILE* pipe = popen(cmd.c_str(), "r");
            if (!pipe) return -1;
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) *o += buf;
            // An lvalue: WEXITSTATUS takes the address of its argument.
            int status = pclose(pipe);
            return WEXITSTATUS(status);
        },
        fx, /*expectPid=*/false);
}

// `status` asks the same question and answers it as a status report rather
// than an error, so it exits 1 and describes the session instead.
TEST_F(CLITest, CrashedSession_StatusReportsNotRunningAtOnce) {
    DeadSessionFixture fx("crashedstatus", DeadSessionFixture::Crashed);

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " status --json", &output, 5);

    EXPECT_EQ(exitCode, 1) << "Output:\n" << output;
    EXPECT_NE(output.find("not_running"), std::string::npos) << "Output:\n" << output;
    EXPECT_NE(output.find(kGonePid), std::string::npos)
        << "the report must name the pid that is gone. Output:\n" << output;
}

TEST_F(CLITest, CleanlyStoppedSession_StatusReportsNotRunningAtOnce) {
    DeadSessionFixture fx("cleanstopstatus", DeadSessionFixture::CleanlyStopped);

    std::string output;
    int exitCode = runLogosctlWithTimeout(
        "--config-dir " + fx.dir.string() + " status --json", &output, 5);

    // Exit 1, not 0. This reported "not_running" and exited 0 -- the text was
    // right and the exit code said the daemon was fine.
    EXPECT_EQ(exitCode, 1)
        << (exitCode == 124 ? "still running after 5s. " : "")
        << "Output:\n" << output;
    EXPECT_NE(output.find("not_running"), std::string::npos) << "Output:\n" << output;

    // ONE document. `status` formats its own "not running" report, so it must
    // not also let the connect helper print an error envelope -- two JSON
    // objects on stdout is not something a caller can parse.
    int lines = 0;
    for (std::size_t i = 0, n = 0; (n = output.find('\n', i)) != std::string::npos; i = n + 1)
        if (n > i) ++lines;
    EXPECT_EQ(lines, 1) << "expected a single JSON document. Output:\n" << output;
}

static void expectEveryRpcCommandFailsAtOnce(const LogosctlRunner& run,
                                             const DeadSessionFixture& fx,
                                             bool expectPid)
{
    for (const std::string& c : rpcCommands()) {
        SCOPED_TRACE(c);
        std::string output;
        // The 5s kill is the assertion's teeth: exit 124 means the command was
        // still waiting for a reply that is never coming.
        int exitCode = run("--config-dir " + fx.dir.string() + " " + c + " --json",
                           &output);

        EXPECT_EQ(exitCode, 2)
            << (exitCode == 124
                    ? "still running after 5s -- it is sitting out the RPC "
                      "deadline against a daemon that is already gone. "
                    : exitCode == 0
                    ? "exited 0 -- it reported a failed RPC as data. "
                    : "")
            << "Output:\n" << output;
        EXPECT_NE(output.find("NO_DAEMON"), std::string::npos)
            << "Output:\n" << output;
        if (expectPid) {
            EXPECT_NE(output.find(kGonePid), std::string::npos)
                << "the error must name the pid, or there is nothing to act "
                   "on. Output:\n" << output;
        }
    }
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
