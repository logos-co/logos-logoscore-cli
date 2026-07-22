// Daemon-backed integration tests.
//
// Unlike test_cli.cpp (which only pokes the binary in no-daemon / inline
// mode), these spin up a *real* logoscore daemon in the background against
// a real test-module directory and drive it through the client subcommands
// — the same shape as logos-logoscore-py's integration suite.
//
// Coverage:
//   * Error paths (ErrorPathTest): unknown-module load, calling methods on
//     unknown / unloaded modules, unknown method on a loaded module,
//     module-info on an unknown module. One fresh daemon per test (the
//     "module known but not loaded" precondition needs pristine state).
//   * Full test_basic_module API + concurrency (LoadedModuleTest): every
//     Q_INVOKABLE return/parameter type (void, bool, int, QString,
//     LogosResult, QVariant, QJsonArray, QStringList), 0..5-arg fan-out,
//     the async delay helper, the event subscription round-trip via
//     `watch`, and many simultaneous clients hitting one daemon. The
//     whole suite shares ONE daemon (SetUpTestSuite) with the module
//     loaded once — per-test daemons made the check take many minutes.
//     Mirrors logos-logoscore-py/tests/integration/test_basic_module_methods.py
//     and logos-test-modules/test-basic-module — keep them in sync.
//
// Negative `call`/`module-info` cases: this CLI/SDK revision has no fast
// "not loaded / not found" guard — the request rides the SDK's ~100s RPC
// timeout (surfacing as RPC_FAILED / non-zero exit, the exact code not
// stable across revisions). So those are `timeout`-bounded and asserted
// as "must not succeed" rather than waiting ~100s for an exact code.
//
// Requires LOGOSCORE_BINARY + LOGOSCORE_TEST_MODULES_DIR (the flake's
// `tests` check wires both, plus LOGOS_HOST_PATH so modules can load).
// Absent ⇒ everything GTEST_SKIPs so the suite stays green locally.

#include <gtest/gtest.h>

#include <logos_json.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p)
{
    std::ifstream ifs(p);
    if (!ifs) return {};
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

// Pull the last well-formed JSON object out of captured client output.
// `call --json` prints one compact envelope line on stdout, but a stray
// qWarning on stderr (captured via 2>&1) could precede it — scan from
// the end for the first line that parses as a JSON object.
nlohmann::json lastJsonObject(const std::string& out)
{
    std::vector<std::string> lines;
    std::string line;
    for (char c : out) {
        if (c == '\n') { lines.push_back(line); line.clear(); }
        else line += c;
    }
    if (!line.empty()) lines.push_back(line);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->empty()) continue;
        try {
            nlohmann::json d = nlohmann::json::parse(*it);
            if (d.is_object()) return d;
        } catch (...) {}
    }
    return nlohmann::json::object();
}

// A real logoscore daemon in an isolated config/HOME, plus helpers to
// drive clients against it. Not a gtest fixture so it can be owned
// per-test (error paths) or once per suite (the API/concurrency matrix).
class LogoscoreDaemon {
public:
    bool envReady(std::string& why) {
        const char* b = std::getenv("LOGOSCORE_BINARY");
        const char* m = std::getenv("LOGOSCORE_TEST_MODULES_DIR");
        if (!b || !fs::exists(b)) { why = "LOGOSCORE_BINARY not set/found"; return false; }
        if (!m || !fs::exists(m)) { why = "LOGOSCORE_TEST_MODULES_DIR not set/found"; return false; }
        binary     = fs::canonical(b);
        modulesDir = fs::canonical(m);
        return true;
    }

    void start(const std::string& tag) {
        base      = fs::temp_directory_path() / ("logoscore_it_" + tag + "_" + std::to_string(getpid()));
        configDir = base / "config";
        homeDir   = base / "home";
        daemonLog = base / "daemon.log";
        fs::create_directories(configDir);
        fs::create_directories(homeDir);
        if (!socketDir.empty()) fs::create_directories(socketDir);
        pid = spawnBg({"daemon", "-m", modulesDir.string()}, daemonLog);
    }

    // fork + setsid + exec a logoscore subprocess (daemon or watch) with
    // this daemon's isolated env; stdout+stderr → logFile, stdin
    // detached. setsid ⇒ the pid leads a process group so the whole
    // tree (incl. logos_host children) tears down together.
    pid_t spawnBg(const std::vector<std::string>& cliArgs, const fs::path& logFile) {
        pid_t p = fork();
        if (p < 0) return -1;
        if (p == 0) {
            setsid();
            setenv("LOGOSCORE_CONFIG_DIR", configDir.c_str(), 1);
            setenv("HOME", homeDir.c_str(), 1);
            // QLocalServer resolves a bare server name against QDir::tempPath(),
            // which honours $TMPDIR — so this decides where the node's sockets
            // land. Tests that assert on socket files set it to get a private
            // directory instead of sharing the machine-wide temp dir (where a
            // dev box's leftovers would swamp the assertions).
            if (!socketDir.empty()) setenv("TMPDIR", socketDir.c_str(), 1);
            FILE* lf = std::fopen(logFile.c_str(), "w");
            if (lf) { dup2(fileno(lf), STDOUT_FILENO); dup2(fileno(lf), STDERR_FILENO); }
            int dn = open("/dev/null", O_RDONLY);
            if (dn >= 0) dup2(dn, STDIN_FILENO);
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>("logoscore"));
            for (const auto& a : cliArgs) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            execv(binary.c_str(), argv.data());
            dprintf(STDERR_FILENO, "execv failed: errno=%d\n", errno);
            _exit(127);
        }
        return p;
    }

    bool waitReady() {
        // Bound each probe: a partially-started daemon (client config
        // written, RPC not yet answering) would otherwise make `status`
        // block the full SDK timeout per iteration, blowing the ~20s
        // readiness budget into minutes.
        for (int i = 0; i < 200; ++i) {
            int st = 0;
            if (pid > 0 && waitpid(pid, &st, WNOHANG) == pid) { pid = -1; return false; }
            if (run("status", nullptr, /*timeoutSecs=*/5) == 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    // Run `logoscore <args> --json` against this daemon. timeoutSecs>0
    // wraps it in coreutils `timeout` (exit 124 if it fires). Safe to
    // call concurrently from multiple threads — each call is its own
    // process and FILE*, sharing no mutable state on this object.
    int run(const std::string& args, std::string* out, int timeoutSecs = 0) const {
        std::string cmd =
            "LOGOSCORE_CONFIG_DIR='" + configDir.string() + "' " +
            "HOME='" + homeDir.string() + "' ";
        // The client dials the same bare socket name, so it must resolve
        // QDir::tempPath() to the same place the daemon bound in.
        if (!socketDir.empty()) cmd += "TMPDIR='" + socketDir.string() + "' ";
        if (timeoutSecs > 0) cmd += "timeout " + std::to_string(timeoutSecs) + " ";
        cmd += "'" + binary.string() + "' " + args + " --json 2>&1";
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return -1;
        char buf[256];
        std::string captured;
        while (fgets(buf, sizeof(buf), pipe)) captured += buf;
        if (out) *out = captured;
        int status = pclose(pipe);
        return WEXITSTATUS(status);
    }

    void killGroup(pid_t p) {
        if (p <= 0) return;
        kill(-p, SIGTERM);
        for (int i = 0; i < 30; ++i) {
            int st = 0;
            if (waitpid(p, &st, WNOHANG) == p) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(-p, SIGKILL);
        int st = 0;
        waitpid(p, &st, 0);
    }

    void shutdown() {
        killGroup(pid);
        pid = -1;
        std::error_code ec;
        if (!base.empty()) fs::remove_all(base, ec);
    }

    fs::path binary, modulesDir, base, configDir, homeDir, daemonLog;
    // Optional private socket directory ($TMPDIR for the node). Empty ⇒
    // inherit the ambient temp dir, which is what every non-socket test wants.
    fs::path socketDir;
    pid_t    pid = -1;
};

// ── socket-file helpers (shared by the socket-lifecycle tests) ─────────────

// Unix socket paths are hard-capped by sockaddr_un::sun_path — 104 bytes on
// macOS, 108 on Linux — and the node appends "/logos_<module>_<12 hex>" to
// $TMPDIR. The longest name in play is "logos_capability_module_" + 12 hex
// = 36 chars, so the directory itself has to stay well under the cap or every
// listen() fails with HostNotFoundError.
//
// macOS's default temp dir (/var/folders/<18 chars>/<18 chars>/T) is already
// ~50 chars, and a per-test subdirectory under it overflows — so prefer the
// ambient temp dir only while it fits, and fall back to a short /tmp path.
// Returns an empty path when neither fits, which the fixture turns into a skip
// rather than a confusing bind failure.
constexpr std::size_t kSunPathCap  = 104;  // the stricter of the two platforms
constexpr std::size_t kLongestName = 40;   // "/logos_capability_module_<12hex>" + slack

fs::path shortSocketDir(const std::string& tag)
{
    const std::string leaf = "lsit_" + std::to_string(getpid()) + "_" + tag;
    for (const fs::path& parent : {fs::temp_directory_path(), fs::path("/tmp")}) {
        const fs::path cand = parent / leaf;
        if (cand.string().size() + kLongestName < kSunPathCap) return cand;
    }
    return {};
}

// Names of the unix-socket files in `dir` starting with "logos_".
// Only S_ISSOCK entries count, so a regular file sharing the prefix is
// excluded from the tally the assertions are written against.
std::vector<std::string> socketNames(const fs::path& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("logos_", 0) != 0) continue;
        struct stat st{};
        if (::lstat(e.path().c_str(), &st) != 0) continue;
        if (S_ISSOCK(st.st_mode)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Bind a unix socket at `p` and listen. Returns the fd (>=0) or -1.
int bindListen(const fs::path& p)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string s = p.string();
    if (s.size() >= sizeof(addr.sun_path)) { ::close(fd); return -1; }
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s.c_str());
    ::unlink(s.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Reap a spawned subprocess (e.g. `watch`) on every exit path —
// including a fatal gtest assertion that `return`s out of the test —
// so background watchers can't leak past the test.
struct ProcGuard {
    LogoscoreDaemon* d;
    pid_t pid;
    ~ProcGuard() { if (d && pid > 0) d->killGroup(pid); }
};

constexpr int kNegativeBudgetSecs = 12;

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Error paths — fresh daemon per test (needs pristine "not loaded" state)
// ═══════════════════════════════════════════════════════════════════════════

class ErrorPathTest : public ::testing::Test {
protected:
    LogoscoreDaemon d;

    void SetUp() override {
        std::string why;
        if (!d.envReady(why)) GTEST_SKIP() << why;
        d.start(::testing::UnitTest::GetInstance()->current_test_info()->name());
        ASSERT_TRUE(d.waitReady())
            << "daemon did not become reachable.\n--- daemon log ---\n"
            << slurp(d.daemonLog);
    }
    void TearDown() override { d.shutdown(); }
};

TEST_F(ErrorPathTest, NoLoadNegativePaths) {
    std::string out;

    // Daemon reachable + module discoverable but not loaded.
    ASSERT_EQ(d.run("status", &out), 0) << out;
    ASSERT_EQ(d.run("list-modules", &out), 0) << out;
    EXPECT_NE(out.find("test_basic"), std::string::npos)
        << "test_basic_module should be discoverable.\n" << out;
    ASSERT_EQ(d.run("list-modules --loaded", &out), 0) << out;
    EXPECT_EQ(out.find("test_basic_module"), std::string::npos)
        << "module must start unloaded.\n" << out;

    // Loading an unknown module must fail.
    EXPECT_NE(d.run("load-module definitely_not_a_real_module_xyz",
                     &out, kNegativeBudgetSecs), 0)
        << "load-module unknown should not succeed.\n" << out;

    // Calling a method on an unknown module must fail.
    EXPECT_NE(d.run("call definitely_not_a_real_module_xyz whatever",
                     &out, kNegativeBudgetSecs), 0)
        << "call on unknown module should not succeed.\n" << out;

    // Calling a method on a known-but-unloaded module must fail.
    EXPECT_NE(d.run("call test_basic_module returnTrue",
                     &out, kNegativeBudgetSecs), 0)
        << "call on unloaded module should not succeed.\n" << out;

    // module-info on an unknown module must fail.
    EXPECT_NE(d.run("module-info definitely_not_a_real_module_xyz",
                     &out, kNegativeBudgetSecs), 0)
        << "module-info on unknown module should not succeed.\n" << out;
}

// Regression for #59: the version, sourced from each module's embedded
// metadata, must appear in list-modules / module-info / load-module — and,
// critically, for modules that are merely KNOWN (not yet loaded), since that
// reads from on-disk metadata rather than a running plugin. test_basic_module
// declares version 1.0.0 in its metadata.json.
TEST_F(ErrorPathTest, ReportsModuleVersion) {
    std::string out;
    const std::string kVersion = "\"version\":\"1.0.0\"";

    // Unloaded module: version is read from metadata, so it is present even
    // before the module is loaded.
    ASSERT_EQ(d.run("list-modules", &out), 0) << out;
    EXPECT_NE(out.find("test_basic_module"), std::string::npos) << out;
    EXPECT_NE(out.find(kVersion), std::string::npos)
        << "list-modules must report the metadata version for a known module.\n"
        << out;

    ASSERT_EQ(d.run("module-info test_basic_module", &out), 0) << out;
    EXPECT_NE(out.find(kVersion), std::string::npos)
        << "module-info must report the module version.\n" << out;
    // module-info is now backed by the generic modules-info dump, so the
    // dependency graph is reported too (test_basic_module has no deps).
    EXPECT_NE(out.find("\"dependencies\""), std::string::npos)
        << "module-info must include the dependencies array.\n" << out;
    // Uptime is loaded-only: an unloaded module reports no uptime_seconds.
    EXPECT_EQ(out.find("uptime_seconds"), std::string::npos)
        << "unloaded module-info must not report uptime_seconds.\n" << out;

    // Loading it returns the version too, and list-modules keeps reporting it.
    ASSERT_EQ(d.run("load-module test_basic_module", &out, kNegativeBudgetSecs), 0)
        << "test_basic_module must load (LOGOS_HOST_PATH wired?).\n" << out;
    EXPECT_NE(out.find(kVersion), std::string::npos)
        << "load-module response must include the version.\n" << out;

    ASSERT_EQ(d.run("list-modules", &out), 0) << out;
    EXPECT_NE(out.find(kVersion), std::string::npos)
        << "list-modules must still report the version once loaded.\n" << out;

    // Once loaded, uptime is derived from the load timestamp and reported.
    ASSERT_EQ(d.run("module-info test_basic_module", &out), 0) << out;
    EXPECT_NE(out.find("uptime_seconds"), std::string::npos)
        << "loaded module-info must report uptime_seconds.\n" << out;
}

TEST_F(ErrorPathTest, UnknownMethodOnLoadedModule) {
    std::string out;
    ASSERT_EQ(d.run("load-module test_basic_module", &out), 0)
        << "test_basic_module must load (LOGOS_HOST_PATH wired?).\n" << out
        << "\n--- daemon log ---\n" << slurp(d.daemonLog);

    EXPECT_NE(d.run("call test_basic_module thisMethodDoesNotExist",
                     &out, kNegativeBudgetSecs), 0)
        << "unknown method on a loaded module should not succeed.\n" << out;

    // A real method still works fast — proves the failure above was
    // method-scoped, not a wedged daemon.
    ASSERT_EQ(d.run("call test_basic_module returnTrue", &out), 0) << out;
    EXPECT_NE(out.find("true"), std::string::npos) << out;
}

// Crash-isolation: modules run in a separate logos_host subprocess
// (logos_core_start spawns it in remote mode). A faulty module that
// SIGSEGVs must take down only that host process — the logoscore
// daemon itself must keep answering clients. Uses test_basic_module's
// crashOnDemand() (a null-pointer deref). Fresh-daemon-per-test
// because killing the host pollutes shared state for everything else.
//
// "Daemon survived" alone is too weak — a missing/renamed method or
// an error-returning stub also exits non-zero. Belt + braces:
//   (1) precondition: module-info lists crashOnDemand → we're really
//       calling the method we think we are;
//   (2) positive crash evidence: after the call, the daemon observes
//       the host die and flips the module out of "loaded" state
//       (module_manager.cpp's onTerminated → registry.markUnloaded).
//       A method that merely returned an error would leave it loaded.
TEST_F(ErrorPathTest, CrashedModuleDoesNotKillDaemon) {
    std::string out;

    // Module loads and is callable — baseline that the daemon + host
    // are healthy before we intentionally crash one of them.
    ASSERT_EQ(d.run("load-module test_basic_module", &out), 0)
        << "test_basic_module must load before crash test.\n" << out
        << "\n--- daemon log ---\n" << slurp(d.daemonLog);
    ASSERT_EQ(d.run("call test_basic_module returnTrue", &out), 0)
        << "module must be callable pre-crash.\n" << out;
    EXPECT_NE(out.find("true"), std::string::npos) << out;

    // Precondition (1): the method we're about to call really exists
    // on the loaded module — otherwise the "non-zero exit" assertion
    // below would pass for the wrong reason (method-not-found, not
    // host crash). Pulls the methods list from module-info --json.
    ASSERT_EQ(d.run("module-info test_basic_module", &out), 0)
        << "module-info should succeed for a loaded module.\n" << out;
    nlohmann::json pre = lastJsonObject(out);
    ASSERT_EQ(pre.value("status", std::string{}), "loaded")
        << "module should be loaded before the crash.\n" << out;
    nlohmann::json methods = pre.value("methods", nlohmann::json::array());
    bool hasCrash = false;
    for (const auto& v : methods) {
        if (v.value("name", std::string{}) == "crashOnDemand") {
            hasCrash = true;
            break;
        }
    }
    ASSERT_TRUE(hasCrash)
        << "test_basic_module must expose crashOnDemand — otherwise "
           "this test would pass for the wrong reason (method-not-found).\n"
        << out;

    // Crash the host. The RPC will not return cleanly: either the
    // peer dies mid-call (RPC_FAILED, non-zero exit) or the SDK
    // burns its full timeout. `timeout` keeps us bounded in either
    // case — we only assert "must not succeed", not a specific code.
    EXPECT_NE(d.run("call test_basic_module crashOnDemand",
                    &out, kNegativeBudgetSecs), 0)
        << "crashOnDemand must not return success.\n" << out;

    // Positive crash evidence (2): the daemon's SIGCHLD handler runs
    // asynchronously, so poll for up to ~5s waiting to observe the
    // module flip out of "loaded" (markUnloaded). If crashOnDemand
    // had merely returned an error envelope without dying, the host
    // would still be alive and module-info would keep reporting
    // status=="loaded" — failing this assertion.
    std::string lastStatus = "<none>";
    bool unloaded = false;
    for (int i = 0; i < 50; ++i) {
        if (d.run("module-info test_basic_module", &out, /*timeoutSecs=*/5) == 0) {
            lastStatus = lastJsonObject(out).value("status", std::string{});
            if (lastStatus != "loaded") { unloaded = true; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(unloaded)
        << "daemon never observed the host crash — module still reports "
        << "status='" << lastStatus << "' (expected anything but 'loaded'). "
        << "Either crashOnDemand didn't actually crash, or the daemon's "
        << "subprocess supervisor stopped detecting host exits.\n"
        << "--- daemon log ---\n" << slurp(d.daemonLog);

    // Original isolation assertion: a quick `status` round-trip proves
    // the daemon is still serving clients after the host subprocess
    // died. If the daemon went down with the module, `status` would
    // either fail to connect or `timeout` would fire.
    ASSERT_EQ(d.run("status", &out, /*timeoutSecs=*/10), 0)
        << "daemon must survive a module crash.\n" << out
        << "\n--- daemon log ---\n" << slurp(d.daemonLog);

    // `list-modules` exercises a different code path than `status`
    // (catalog scan vs. liveness check) — both must keep working.
    ASSERT_EQ(d.run("list-modules", &out, /*timeoutSecs=*/10), 0)
        << "list-modules must still work after a module crash.\n" << out;
    EXPECT_NE(out.find("test_basic"), std::string::npos)
        << "module must remain discoverable after its host crashed.\n" << out;
}

// Auth-token regression: the client must present a token core_service
// accepts on every RPC.
//
// The daemon issues one `auto` bearer token at boot and registers its raw
// value in its TokenManager (under "cli_client"). core_service validates an
// incoming token *by value* (ModuleProxy::isAuthorized scans every stored
// token), so the client only authenticates if it actually *transmits* that
// token. But the SDK picks which token to send by the TARGET name —
// getToken(objectName) with objectName == "core_service" — so the client
// must have the bearer token filed under the "core_service" key
// (RpcClient::connect, src/client/client.cpp). With that registration
// missing, getToken("core_service") misses, the SDK falls into the
// capability_module requestModule fallback (which can't mint a token for
// the in-process core_service), an unrecognized token reaches the daemon,
// and EVERY business RPC is rejected — list-modules, load-module, call,
// status, stop all fail. (Pre-enforcement SDKs ignored the token, hiding
// this; once ModuleProxy started enforcing, the gap became fatal.)
//
// This test pins that path: a load + a real method call must succeed, AND
// the daemon log must carry no "rejecting unauthorized" line — so a
// regression that breaks the client's token registration fails here loudly,
// not as some unrelated RPC error.
TEST_F(ErrorPathTest, ClientAuthenticatesToCoreService) {
    std::string out;

    // list-modules is the cheapest authenticated core_service RPC (no
    // module load involved). If the client isn't presenting a token
    // core_service accepts, this already fails.
    ASSERT_EQ(d.run("list-modules", &out), 0)
        << "list-modules (an authenticated core_service RPC) must succeed — "
           "the client must present a token core_service accepts.\n" << out
        << "\n--- daemon log ---\n" << slurp(d.daemonLog);

    // A real business dispatch end-to-end: load the module, then call a
    // method on it. Both legs are authenticated core_service RPCs; the
    // second also drives core_service -> module. A broken client token
    // registration makes load-module fail outright.
    ASSERT_EQ(d.run("load-module test_basic_module", &out), 0)
        << "load-module must succeed for an authenticated client "
           "(LOGOS_HOST_PATH wired?).\n" << out
        << "\n--- daemon log ---\n" << slurp(d.daemonLog);

    ASSERT_EQ(d.run("call test_basic_module returnTrue", &out), 0)
        << "an authenticated call must round-trip and succeed.\n" << out
        << "\n--- daemon log ---\n" << slurp(d.daemonLog);
    EXPECT_NE(out.find("true"), std::string::npos) << out;

    // Root-cause pin: prove the calls above weren't authorized by some
    // unrelated accident. core_service logs exactly this string when it
    // rejects an unrecognized token (ModuleProxy::callRemoteMethod). Its
    // presence means the client sent a token core_service didn't accept —
    // i.e. the very regression this test guards against.
    const std::string log = slurp(d.daemonLog);
    EXPECT_EQ(log.find("rejecting unauthorized"), std::string::npos)
        << "core_service rejected a client token as unauthorized — the "
           "client is not presenting a token it accepts (check the "
           "core_service token registration in RpcClient::connect).\n"
           "--- daemon log ---\n" << log;
}

// ═══════════════════════════════════════════════════════════════════════════
// Full API + concurrency — ONE shared daemon, module loaded once
// ═══════════════════════════════════════════════════════════════════════════

class LoadedModuleTest : public ::testing::Test {
protected:
    static LogoscoreDaemon* s_d;
    static bool s_skip;
    static std::string s_skipWhy;

    static void SetUpTestSuite() {
        s_d = new LogoscoreDaemon();
        if (!s_d->envReady(s_skipWhy)) { s_skip = true; return; }
        s_d->start("loaded_suite");
        ASSERT_TRUE(s_d->waitReady())
            << "daemon did not become reachable.\n--- daemon log ---\n"
            << slurp(s_d->daemonLog);
        // Load once for the whole suite. If it fails (e.g. the daemon's
        // modules dir is missing capability_module so the request hangs
        // on capability negotiation), skip the API/concurrency tests
        // with the daemon log attached rather than letting every test
        // burn the ~20s RPC timeout into a hard failure.
        std::string out;
        if (s_d->run("load-module test_basic_module", &out, 30) != 0) {
            s_skip = true;
            s_skipWhy = "test_basic_module failed to load — skipping API/"
                        "concurrency suite.\n" + out +
                        "\n--- daemon log ---\n" + slurp(s_d->daemonLog);
        }
    }

    static void TearDownTestSuite() {
        // shutdown() is a no-op when the daemon was never started
        // (env missing) — pid<=0 and base empty — so it's always safe.
        if (s_d) { s_d->shutdown(); delete s_d; s_d = nullptr; }
    }

    void SetUp() override {
        if (s_skip) GTEST_SKIP() << s_skipWhy;
    }

    // `call test_basic_module <method> [args]` → `result` of the success
    // envelope {"status":"ok","module":...,"result":<v>}.
    nlohmann::json call(const std::string& method, const std::string& args = "") {
        std::string out;
        const std::string cmd =
            "call test_basic_module " + method + (args.empty() ? "" : " " + args);
        EXPECT_EQ(s_d->run(cmd, &out), 0) << cmd << "\n" << out;
        nlohmann::json env = lastJsonObject(out);
        EXPECT_EQ(env.value("status", std::string{}), "ok")
            << cmd << "\n" << out;
        return env.value("result", nlohmann::json{});
    }
};

LogoscoreDaemon* LoadedModuleTest::s_d = nullptr;
bool LoadedModuleTest::s_skip = false;
std::string LoadedModuleTest::s_skipWhy;

// ── void / bool / int (void surfaces as `true` — call_executor.cpp) ──────────

TEST_F(LoadedModuleTest, VoidAndBoolReturns) {
    EXPECT_TRUE(call("doNothing").get<bool>());
    EXPECT_TRUE(call("doNothingWithArgs", "hello 7").get<bool>());
    EXPECT_TRUE(call("returnTrue").get<bool>());
    EXPECT_FALSE(call("returnFalse").get<bool>());
    EXPECT_TRUE(call("isPositive", "5").get<bool>());
    EXPECT_FALSE(call("isPositive", "-3").get<bool>());
    EXPECT_FALSE(call("isPositive", "0").get<bool>());
}

TEST_F(LoadedModuleTest, IntReturns) {
    EXPECT_EQ(call("returnInt").get<int>(), 42);
    EXPECT_EQ(call("addInts", "2 3").get<int>(), 5);
    EXPECT_EQ(call("stringLength", "abcdef").get<int>(), 6);
    EXPECT_EQ(call("echoInt", "123").get<int>(), 123);
    EXPECT_EQ(call("byteArraySize", "abcde").get<int>(), 5);
}

TEST_F(LoadedModuleTest, StringReturns) {
    EXPECT_EQ(call("returnString").get<std::string>(), "test_basic_module");
    EXPECT_EQ(call("echo", "roundtrip").get<std::string>(), "roundtrip");
    EXPECT_EQ(call("concat", "foo bar").get<std::string>(), "foobar");
    EXPECT_EQ(call("urlToString", "https://example.com/p").get<std::string>(),
              "https://example.com/p");
}

TEST_F(LoadedModuleTest, LogosResultShapes) {
    nlohmann::json ok = call("successResult");
    EXPECT_TRUE(ok["success"].get<bool>());
    EXPECT_EQ(ok["value"].get<std::string>(), "operation succeeded");
    EXPECT_TRUE(ok["error"].is_null());

    nlohmann::json err = call("errorResult");
    EXPECT_FALSE(err["success"].get<bool>());
    EXPECT_TRUE(err["value"].is_null());
    EXPECT_EQ(err["error"].get<std::string>(), "deliberate error for testing");

    nlohmann::json m = call("resultWithMap")["value"];
    EXPECT_EQ(m["name"].get<std::string>(), "test");
    EXPECT_EQ(m["count"].get<int>(), 42);
    EXPECT_TRUE(m["active"].get<bool>());

    nlohmann::json lst = call("resultWithList")["value"];
    ASSERT_EQ(lst.size(), 2u);
    EXPECT_EQ(lst[0]["label"].get<std::string>(), "first");
    EXPECT_EQ(lst[1]["id"].get<int>(), 2);

    nlohmann::json vOk = call("validateInput", "hello");
    EXPECT_TRUE(vOk["success"].get<bool>());
    EXPECT_EQ(vOk["value"]["length"].get<int>(), 5);

    nlohmann::json vErr = call("validateInput", "''");
    EXPECT_FALSE(vErr["success"].get<bool>());
    EXPECT_EQ(vErr["error"].get<std::string>(), "input cannot be empty");
}

TEST_F(LoadedModuleTest, VariantAndCollectionReturns) {
    EXPECT_EQ(call("returnVariantInt").get<int>(), 99);
    EXPECT_EQ(call("returnVariantString").get<std::string>(), "variant_string");

    nlohmann::json vm = call("returnVariantMap");
    EXPECT_EQ(vm["key"].get<std::string>(), "value");
    EXPECT_EQ(vm["number"].get<int>(), 7);

    nlohmann::json vl = call("returnVariantList");
    ASSERT_EQ(vl.size(), 3u);
    EXPECT_EQ(vl[0].get<std::string>(), "alpha");
    EXPECT_EQ(vl[2].get<std::string>(), "gamma");

    nlohmann::json ja = call("returnJsonArray");
    ASSERT_EQ(ja.size(), 3u);
    EXPECT_EQ(ja[0].get<int>(), 1);
    EXPECT_EQ(ja[2].get<int>(), 3);

    nlohmann::json mk = call("makeJsonArray", "x y");
    ASSERT_EQ(mk.size(), 2u);
    EXPECT_EQ(mk[1].get<std::string>(), "y");

    nlohmann::json sl = call("returnStringList");
    ASSERT_EQ(sl.size(), 3u);
    EXPECT_EQ(sl[1].get<std::string>(), "two");

    nlohmann::json sp = call("splitString", "a,b,c");
    ASSERT_EQ(sp.size(), 3u);
    EXPECT_EQ(sp[0].get<std::string>(), "a");
    EXPECT_EQ(sp[2].get<std::string>(), "c");
}

TEST_F(LoadedModuleTest, ArgCountFanOut) {
    EXPECT_EQ(call("noArgs").get<std::string>(), "noArgs()");
    EXPECT_EQ(call("oneArg", "x").get<std::string>(), "oneArg(x)");
    EXPECT_EQ(call("twoArgs", "x 7").get<std::string>(), "twoArgs(x, 7)");
    EXPECT_EQ(call("threeArgs", "x 7 true").get<std::string>(),
              "threeArgs(x, 7, true)");
    EXPECT_EQ(call("fourArgs", "x 7 false y").get<std::string>(),
              "fourArgs(x, 7, false, y)");
    EXPECT_EQ(call("fiveArgs", "x 7 true y 9").get<std::string>(),
              "fiveArgs(x, 7, true, y, 9)");
    EXPECT_TRUE(call("echoBool", "true").get<bool>());
    EXPECT_FALSE(call("echoBool", "false").get<bool>());
}

TEST_F(LoadedModuleTest, AsyncEchoWithDelay) {
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(call("echoWithDelay", "pong 200").get<std::string>(), "pong");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    EXPECT_GE(ms, 200) << "echoWithDelay returned too fast (" << ms << "ms)";
}

// ── Events: subscribe via `watch`, fire via a method call ────────────────────

// Re-emit the event on a cadence while polling the watch log instead of
// sleeping a fixed time and emitting once: emitting is idempotent, so
// this races neither the watcher's subscription nor a slow CI worker —
// whenever the subscription becomes active, a subsequent emit lands.
// The ProcGuard reaps the watcher even if a fatal assertion fires.

TEST_F(LoadedModuleTest, EmitTestEventRoundTrip) {
    const fs::path log = s_d->base / "watch_single.log";
    pid_t w = s_d->spawnBg({"watch", "test_basic_module", "--event", "testEvent"}, log);
    ASSERT_GT(w, 0);
    ProcGuard guard{s_d, w};

    // Give the watcher time to connect and register its subscription
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) {  // up to ~30s
        std::string out;
        s_d->run("call test_basic_module emitTestEvent payload123", &out);
        if (slurp(log).find("payload123") != std::string::npos) { got = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(got) << "testEvent payload not observed.\n--- watch log ---\n"
                     << slurp(log);
}

TEST_F(LoadedModuleTest, EmitMultiArgEvent) {
    const fs::path log = s_d->base / "watch_multi.log";
    pid_t w = s_d->spawnBg({"watch", "test_basic_module", "--event", "multiArgEvent"}, log);
    ASSERT_GT(w, 0);
    ProcGuard guard{s_d, w};

    // Give the watcher time to connect and register its subscription
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) {  // up to ~30s
        std::string out;
        s_d->run("call test_basic_module emitMultiArgEvent label 42", &out);
        const std::string l = slurp(log);
        if (l.find("label") != std::string::npos && l.find("42") != std::string::npos) {
            got = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(got) << "multiArgEvent not observed.\n--- watch log ---\n" << slurp(log);
}

// ── Concurrency: many independent clients hitting one daemon at once ─────────
// Each thread is its own `logoscore call` process. Worker threads do NO
// gtest assertions (not thread-safe) — they record (ok, detail) into
// private slots; the main thread asserts after join.

TEST_F(LoadedModuleTest, ConcurrentEchoFromManyClients) {
    constexpr int N = 12;
    std::vector<std::thread> ts;
    std::vector<int> ok(N, 0);
    std::vector<std::string> detail(N);

    for (int i = 0; i < N; ++i) {
        ts.emplace_back([&, i] {
            const std::string tok = "tok" + std::to_string(i);
            std::string out;
            int rc = s_d->run("call test_basic_module echo " + tok, &out);
            std::string got;
            try {
                got = lastJsonObject(out)["result"].get<std::string>();
            } catch (...) {}
            ok[i] = (rc == 0 && got == tok) ? 1 : 0;
            if (!ok[i]) detail[i] = "rc=" + std::to_string(rc) +
                                    " got='" + got + "' want='" + tok + "'\n" + out;
        });
    }
    for (auto& t : ts) t.join();

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(ok[i], 1) << "client " << i << ": " << detail[i];
}

TEST_F(LoadedModuleTest, ConcurrentMixedMethodsFromManyClients) {
    constexpr int N = 16;
    std::vector<std::thread> ts;
    std::vector<int> ok(N, 0);
    std::vector<std::string> detail(N);

    for (int i = 0; i < N; ++i) {
        ts.emplace_back([&, i] {
            std::string out;
            int rc = -1;
            bool good = false;
            try {
                switch (i % 4) {
                case 0: {  // addInts(i, i) == 2*i
                    rc = s_d->run("call test_basic_module addInts " +
                                  std::to_string(i) + " " + std::to_string(i), &out);
                    good = lastJsonObject(out)["result"].get<int>() == 2 * i;
                    break;
                }
                case 1: {  // echoInt(i) == i
                    rc = s_d->run("call test_basic_module echoInt " + std::to_string(i), &out);
                    good = lastJsonObject(out)["result"].get<int>() == i;
                    break;
                }
                case 2: {  // returnString() == "test_basic_module"
                    rc = s_d->run("call test_basic_module returnString", &out);
                    good = lastJsonObject(out)["result"].get<std::string>() == "test_basic_module";
                    break;
                }
                default: {  // stringLength("xxxx..i..") == i
                    rc = s_d->run("call test_basic_module stringLength " +
                                  std::string(static_cast<size_t>(i), 'x'), &out);
                    good = lastJsonObject(out)["result"].get<int>() == i;
                    break;
                }
                }
            } catch (...) {}
            ok[i] = (rc == 0 && good) ? 1 : 0;
            if (!ok[i]) detail[i] = "case=" + std::to_string(i % 4) +
                                    " rc=" + std::to_string(rc) + "\n" + out;
        });
    }
    for (auto& t : ts) t.join();

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(ok[i], 1) << "client " << i << ": " << detail[i];
}

// ═══════════════════════════════════════════════════════════════════════════
// Socket lifecycle — the node must not leave unix-socket files behind
//
// The reported symptom was dozens of stale /tmp/logos_* files: the module
// host died instantly on SIGTERM, so QCoreApplication::exec() never returned
// and QLocalServer's destructor — the only thing that unlinks the socket —
// never ran. Every clean shutdown leaked one file per module.
//
// Two halves, because they fail independently:
//   * a graceful stop unlinks everything it bound (the shutdown handler);
//   * whatever a *hard* kill leaves behind is reaped at the next boot (the
//     reaper), which is the only path available for SIGKILL / crashes.
//
// Both run in a private $TMPDIR so the assertions describe this node's
// sockets and not a developer box's accumulated leftovers.
// ═══════════════════════════════════════════════════════════════════════════

class SocketLifecycleTest : public ::testing::Test {
protected:
    LogoscoreDaemon d;

    void SetUp() override {
        std::string why;
        if (!d.envReady(why)) GTEST_SKIP() << why;
        d.socketDir = shortSocketDir(
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
        if (d.socketDir.empty())
            GTEST_SKIP() << "no temp directory short enough for AF_UNIX sun_path";
        std::error_code ec;
        fs::remove_all(d.socketDir, ec);
        fs::create_directories(d.socketDir, ec);
        if (ec) GTEST_SKIP() << "cannot create " << d.socketDir << ": " << ec.message();
    }

    void TearDown() override {
        d.shutdown();
        std::error_code ec;
        fs::remove_all(d.socketDir, ec);
    }
};

TEST_F(SocketLifecycleTest, NoSocketsSurviveGracefulStop)
{
    d.start("sockets_stop");
    ASSERT_TRUE(d.waitReady()) << slurp(d.daemonLog);

    // Load a module so the tally covers a logos_host child socket too — that
    // subprocess is a separate binary with its own shutdown path, and it is
    // where the bulk of the reported leak came from.
    std::string out;
    ASSERT_EQ(d.run("load-module test_basic_module", &out), 0) << out;

    const auto live = socketNames(d.socketDir);
    // core_service + capability_module + test_basic_module.
    ASSERT_GE(live.size(), 2u)
        << "expected the running node to have bound sockets in " << d.socketDir
        << "; found " << live.size() << ". Without this the test would pass "
        << "vacuously.\n" << slurp(d.daemonLog);

    ASSERT_EQ(d.run("stop", &out), 0) << out;

    // The daemon acks `stop` before its children have finished unwinding, so
    // give the module hosts a bounded moment to run their destructors.
    std::vector<std::string> remaining;
    for (int i = 0; i < 100; ++i) {
        remaining = socketNames(d.socketDir);
        if (remaining.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string leaked;
    for (const auto& n : remaining) leaked += "\n  " + n;
    EXPECT_TRUE(remaining.empty())
        << "sockets survived a graceful stop:" << leaked << "\n"
        << slurp(d.daemonLog);
}

TEST_F(SocketLifecycleTest, BootReapsStaleSocketsButSparesLiveOnesAndFiles)
{
    fs::create_directories(d.socketDir);

    // A *stale* socket: bound, then the listener closed. The path stays on
    // disk and connect() is refused — exactly what a SIGKILLed node leaves.
    const fs::path stale = d.socketDir / "logos_stale_aaaaaaaaaaaa";
    const int staleFd = bindListen(stale);
    ASSERT_GE(staleFd, 0) << "could not bind " << stale;
    ::close(staleFd);
    ASSERT_TRUE(fs::exists(stale));

    // A *live* socket owned by this test process, held open for the duration.
    // Reaping it would mean the reaper can kill a co-resident node's endpoints.
    const fs::path live = d.socketDir / "logos_live_bbbbbbbbbbbb";
    const int liveFd = bindListen(live);
    ASSERT_GE(liveFd, 0) << "could not bind " << live;

    // A regular file that merely shares the prefix. A glob-based cleanup would
    // delete it; the real one only ever unlinks S_ISSOCK inodes. (Not
    // hypothetical: a multi-hundred-MB logos_*.lgx sits in the temp dir on a
    // dev box.)
    const fs::path plain = d.socketDir / "logos_execution_zone-1.0.0.lgx";
    { std::ofstream f(plain); f << "not a socket"; }
    ASSERT_TRUE(fs::exists(plain));

    d.start("sockets_reap");
    const bool ready = d.waitReady();
    ::close(liveFd);
    ASSERT_TRUE(ready) << slurp(d.daemonLog);

    EXPECT_FALSE(fs::exists(stale))
        << "a stale socket survived the boot reaper: " << stale << "\n"
        << slurp(d.daemonLog);
    EXPECT_TRUE(fs::exists(live))
        << "the reaper unlinked a LIVE socket — a co-resident node would lose "
        << "its endpoint: " << live;
    ASSERT_TRUE(fs::exists(plain))
        << "the reaper deleted a regular file sharing the prefix: " << plain;
    EXPECT_EQ(slurp(plain), "not a socket") << "regular file was modified";
}
