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

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

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
QJsonObject lastJsonObject(const std::string& out)
{
    std::vector<std::string> lines;
    std::string line;
    for (char c : out) {
        if (c == '\n') { lines.push_back(line); line.clear(); }
        else line += c;
    }
    if (!line.empty()) lines.push_back(line);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        QJsonParseError pe;
        QJsonDocument d =
            QJsonDocument::fromJson(QByteArray::fromStdString(*it), &pe);
        if (pe.error == QJsonParseError::NoError && d.isObject())
            return d.object();
    }
    return {};
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
    pid_t    pid = -1;
};

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
    QJsonObject pre = lastJsonObject(out);
    ASSERT_EQ(pre.value("status").toString().toStdString(), "loaded")
        << "module should be loaded before the crash.\n" << out;
    QJsonArray methods = pre.value("methods").toArray();
    bool hasCrash = false;
    for (const QJsonValue& v : methods) {
        if (v.toObject().value("name").toString() == "crashOnDemand") {
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
            lastStatus = lastJsonObject(out)
                             .value("status").toString().toStdString();
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
    QJsonValue call(const std::string& method, const std::string& args = "") {
        std::string out;
        const std::string cmd =
            "call test_basic_module " + method + (args.empty() ? "" : " " + args);
        EXPECT_EQ(s_d->run(cmd, &out), 0) << cmd << "\n" << out;
        QJsonObject env = lastJsonObject(out);
        EXPECT_EQ(env.value("status").toString().toStdString(), "ok")
            << cmd << "\n" << out;
        return env.value("result");
    }
};

LogoscoreDaemon* LoadedModuleTest::s_d = nullptr;
bool LoadedModuleTest::s_skip = false;
std::string LoadedModuleTest::s_skipWhy;

// ── void / bool / int (void surfaces as `true` — call_executor.cpp) ──────────

TEST_F(LoadedModuleTest, VoidAndBoolReturns) {
    EXPECT_TRUE(call("doNothing").toBool());
    EXPECT_TRUE(call("doNothingWithArgs", "hello 7").toBool());
    EXPECT_TRUE(call("returnTrue").toBool());
    EXPECT_FALSE(call("returnFalse").toBool());
    EXPECT_TRUE(call("isPositive", "5").toBool());
    EXPECT_FALSE(call("isPositive", "-3").toBool());
    EXPECT_FALSE(call("isPositive", "0").toBool());
}

TEST_F(LoadedModuleTest, IntReturns) {
    EXPECT_EQ(call("returnInt").toInt(), 42);
    EXPECT_EQ(call("addInts", "2 3").toInt(), 5);
    EXPECT_EQ(call("stringLength", "abcdef").toInt(), 6);
    EXPECT_EQ(call("echoInt", "123").toInt(), 123);
    EXPECT_EQ(call("byteArraySize", "abcde").toInt(), 5);
}

TEST_F(LoadedModuleTest, StringReturns) {
    EXPECT_EQ(call("returnString").toString().toStdString(), "test_basic_module");
    EXPECT_EQ(call("echo", "roundtrip").toString().toStdString(), "roundtrip");
    EXPECT_EQ(call("concat", "foo bar").toString().toStdString(), "foobar");
    EXPECT_EQ(call("urlToString", "https://example.com/p")
                  .toString().toStdString(), "https://example.com/p");
}

TEST_F(LoadedModuleTest, LogosResultShapes) {
    QJsonObject ok = call("successResult").toObject();
    EXPECT_TRUE(ok.value("success").toBool());
    EXPECT_EQ(ok.value("value").toString().toStdString(), "operation succeeded");
    EXPECT_TRUE(ok.value("error").isNull());

    QJsonObject err = call("errorResult").toObject();
    EXPECT_FALSE(err.value("success").toBool());
    EXPECT_TRUE(err.value("value").isNull());
    EXPECT_EQ(err.value("error").toString().toStdString(),
              "deliberate error for testing");

    QJsonObject m = call("resultWithMap").toObject().value("value").toObject();
    EXPECT_EQ(m.value("name").toString().toStdString(), "test");
    EXPECT_EQ(m.value("count").toInt(), 42);
    EXPECT_TRUE(m.value("active").toBool());

    QJsonArray lst = call("resultWithList").toObject().value("value").toArray();
    ASSERT_EQ(lst.size(), 2);
    EXPECT_EQ(lst[0].toObject().value("label").toString().toStdString(), "first");
    EXPECT_EQ(lst[1].toObject().value("id").toInt(), 2);

    QJsonObject vOk = call("validateInput", "hello").toObject();
    EXPECT_TRUE(vOk.value("success").toBool());
    EXPECT_EQ(vOk.value("value").toObject().value("length").toInt(), 5);

    QJsonObject vErr = call("validateInput", "''").toObject();
    EXPECT_FALSE(vErr.value("success").toBool());
    EXPECT_EQ(vErr.value("error").toString().toStdString(), "input cannot be empty");
}

TEST_F(LoadedModuleTest, VariantAndCollectionReturns) {
    EXPECT_EQ(call("returnVariantInt").toInt(), 99);
    EXPECT_EQ(call("returnVariantString").toString().toStdString(), "variant_string");

    QJsonObject vm = call("returnVariantMap").toObject();
    EXPECT_EQ(vm.value("key").toString().toStdString(), "value");
    EXPECT_EQ(vm.value("number").toInt(), 7);

    QJsonArray vl = call("returnVariantList").toArray();
    ASSERT_EQ(vl.size(), 3);
    EXPECT_EQ(vl[0].toString().toStdString(), "alpha");
    EXPECT_EQ(vl[2].toString().toStdString(), "gamma");

    QJsonArray ja = call("returnJsonArray").toArray();
    ASSERT_EQ(ja.size(), 3);
    EXPECT_EQ(ja[0].toInt(), 1);
    EXPECT_EQ(ja[2].toInt(), 3);

    QJsonArray mk = call("makeJsonArray", "x y").toArray();
    ASSERT_EQ(mk.size(), 2);
    EXPECT_EQ(mk[1].toString().toStdString(), "y");

    QJsonArray sl = call("returnStringList").toArray();
    ASSERT_EQ(sl.size(), 3);
    EXPECT_EQ(sl[1].toString().toStdString(), "two");

    QJsonArray sp = call("splitString", "a,b,c").toArray();
    ASSERT_EQ(sp.size(), 3);
    EXPECT_EQ(sp[0].toString().toStdString(), "a");
    EXPECT_EQ(sp[2].toString().toStdString(), "c");
}

TEST_F(LoadedModuleTest, ArgCountFanOut) {
    EXPECT_EQ(call("noArgs").toString().toStdString(), "noArgs()");
    EXPECT_EQ(call("oneArg", "x").toString().toStdString(), "oneArg(x)");
    EXPECT_EQ(call("twoArgs", "x 7").toString().toStdString(), "twoArgs(x, 7)");
    EXPECT_EQ(call("threeArgs", "x 7 true").toString().toStdString(),
              "threeArgs(x, 7, true)");
    EXPECT_EQ(call("fourArgs", "x 7 false y").toString().toStdString(),
              "fourArgs(x, 7, false, y)");
    EXPECT_EQ(call("fiveArgs", "x 7 true y 9").toString().toStdString(),
              "fiveArgs(x, 7, true, y, 9)");
    EXPECT_TRUE(call("echoBool", "true").toBool());
    EXPECT_FALSE(call("echoBool", "false").toBool());
}

TEST_F(LoadedModuleTest, AsyncEchoWithDelay) {
    auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(call("echoWithDelay", "pong 200").toString().toStdString(), "pong");
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
            const std::string got =
                lastJsonObject(out).value("result").toString().toStdString();
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
            switch (i % 4) {
            case 0: {  // addInts(i, i) == 2*i
                rc = s_d->run("call test_basic_module addInts " +
                              std::to_string(i) + " " + std::to_string(i), &out);
                good = lastJsonObject(out).value("result").toInt() == 2 * i;
                break;
            }
            case 1: {  // echoInt(i) == i
                rc = s_d->run("call test_basic_module echoInt " + std::to_string(i), &out);
                good = lastJsonObject(out).value("result").toInt() == i;
                break;
            }
            case 2: {  // returnString() == "test_basic_module"
                rc = s_d->run("call test_basic_module returnString", &out);
                good = lastJsonObject(out).value("result").toString() == "test_basic_module";
                break;
            }
            default: {  // stringLength("xxxx..i..") == i
                rc = s_d->run("call test_basic_module stringLength " +
                              std::string(static_cast<size_t>(i), 'x'), &out);
                good = lastJsonObject(out).value("result").toInt() == i;
                break;
            }
            }
            ok[i] = (rc == 0 && good) ? 1 : 0;
            if (!ok[i]) detail[i] = "case=" + std::to_string(i % 4) +
                                    " rc=" + std::to_string(rc) + "\n" + out;
        });
    }
    for (auto& t : ts) t.join();

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(ok[i], 1) << "client " << i << ": " << detail[i];
}
