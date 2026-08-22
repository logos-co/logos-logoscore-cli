// Unit tests for src/process_util.h.
//
// waitForProcessExit is the evidence behind `logosctl daemon stop` reporting
// success for a shutdown whose reply never arrived (see RpcClient::shutdown).
// A missing reply is normal there — the daemon is being asked to die — but
// "no reply" alone must never be read as "it died", so the whole distinction
// rests on this function answering honestly in both directions.

#include <gtest/gtest.h>

#include <process_util.h>

#include <chrono>
#include <csignal>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>

namespace {

int elapsedMs(std::chrono::steady_clock::time_point t0)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
}

// Spawn a child that exits after `lifetimeMs`, with SIGCHLD ignored so the
// kernel reaps it for us. Without that the child lingers as a zombie, and
// kill(pid, 0) — what processAlive asks — reports a zombie as ALIVE. The real
// caller is never the daemon's parent, so it never sees one; this test would.
struct AutoReapedChild {
    struct sigaction saved{};
    pid_t pid = -1;

    explicit AutoReapedChild(int lifetimeMs) {
        struct sigaction ign{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        sigaction(SIGCHLD, &ign, &saved);

        pid = ::fork();
        if (pid == 0) {
            ::usleep(static_cast<useconds_t>(lifetimeMs) * 1000);
            ::_exit(0);
        }
    }
    ~AutoReapedChild() {
        if (pid > 0) ::kill(pid, SIGKILL);
        sigaction(SIGCHLD, &saved, nullptr);
    }
};

} // namespace

TEST(ProcessUtil, WaitForProcessExitRejectsNonPids)
{
    // A daemon state file with no pid in it must not be mistaken for a daemon
    // that has exited — that would turn "I have no idea" into "all good".
    EXPECT_FALSE(logosctl::waitForProcessExit(0, 50));
    EXPECT_FALSE(logosctl::waitForProcessExit(-1, 50));
}

TEST(ProcessUtil, WaitForProcessExitReturnsFalseWhileTheProcessLives)
{
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_FALSE(logosctl::waitForProcessExit(::getpid(), 300));
    // It must actually have waited, not bailed out early on some other read
    // of "not exited yet".
    EXPECT_GE(elapsedMs(t0), 250);
}

TEST(ProcessUtil, WaitForProcessExitNoticesTheExitPromptly)
{
    AutoReapedChild child(200);
    ASSERT_GT(child.pid, 0) << "fork failed";

    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_TRUE(logosctl::waitForProcessExit(child.pid, 10000));
    const int took = elapsedMs(t0);

    // Promptly: the point of polling is that a stop command reports back as
    // soon as the daemon is gone, not when its deadline runs out.
    EXPECT_GE(took, 150) << "returned before the child could possibly have exited";
    EXPECT_LT(took, 3000) << "took " << took << "ms to notice a 200ms child";
    child.pid = -1;   // already gone; nothing to kill
}
