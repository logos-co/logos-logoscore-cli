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

#include <fcntl.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int elapsedMs(std::chrono::steady_clock::time_point t0)
{
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
}

// Spawn a child that exits after `lifetimeMs`, with SIGCHLD ignored so the
// kernel reaps it for us.
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

#ifndef _WIN32
TEST(ProcessUtil, AnUnreapedChildCountsAsExited)
{
    // A daemon whose parent has not reaped it yet, e.g. a test harness's.
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0) << "fork failed";
    if (pid == 0) ::_exit(0);

    siginfo_t info{};
    ASSERT_EQ(::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT), 0);
    ASSERT_EQ(::kill(pid, 0), 0) << "precondition: the exited child is still a zombie";

    EXPECT_FALSE(logosctl::processAlive(pid));
    EXPECT_TRUE(logosctl::waitForProcessExit(pid, 1000));

    int status = 0;
    ::waitpid(pid, &status, 0);
}
#endif

#if defined(__linux__)
// `daemon stop` aborted with "basic_filebuf::underflow error reading the file:
// No such process" when the daemon was reaped between opening /proc/<pid>/stat and reading it.
TEST(ProcessUtil, AStatReadRacingTheReapReportsExitedInsteadOfThrowing)
{
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0) << "fork failed";
    if (pid == 0) { ::pause(); ::_exit(0); }

    const int fd = ::open(("/proc/" + std::to_string(pid) + "/stat").c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);   // reaped: the read below fails with ESRCH

    bool exited = false;
    EXPECT_NO_THROW(exited = logosctl::procStatShowsExited(fd));
    EXPECT_TRUE(exited);
    ::close(fd);
}
#endif
