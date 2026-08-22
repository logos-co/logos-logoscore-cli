#ifndef LOGOS_PROCESS_UTIL_H
#define LOGOS_PROCESS_UTIL_H

// Is a pid from the daemon state file still running?
//
// Two callers depend on this, with OPPOSITE polarity, which is why it is one
// shared function rather than two open-coded checks:
//
//   daemon.cpp          alive  => refuse to start, a node is already up
//   status_command.cpp  dead   => report the state file as stale
//
// Neither may be stubbed on a platform: answering "always alive" makes the
// daemon unrestartable after a crash, and "always dead" lets two daemons run
// concurrently and clobber each other's state.json.
//
// The POSIX contract being reproduced is `kill(pid, 0)`:
//   0        the process exists
//   EPERM    it exists but belongs to another user -- still ALIVE
//   ESRCH    no such process -- dead
// so only ESRCH counts as dead.

#include <chrono>
#include <csignal>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <sys/types.h>
#endif

namespace logosctl {

inline bool processAlive(long long pid)
{
    if (pid <= 0) return false;

#ifdef _WIN32
    // SYNCHRONIZE is what lets us wait on the handle;
    // PROCESS_QUERY_LIMITED_INFORMATION is the least privilege that opens a
    // process owned by the same user without demanding debug rights.
    const HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, static_cast<DWORD>(pid));
    if (h == nullptr) {
        // ERROR_ACCESS_DENIED means the pid EXISTS but is not ours -- the
        // direct analogue of POSIX EPERM, and therefore alive. Anything else
        // (notably ERROR_INVALID_PARAMETER for an unknown pid) is dead.
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    }

    // WaitForSingleObject rather than GetExitCodeProcess: a process handle is
    // signalled exactly when the process has exited, whereas GetExitCodeProcess
    // reports STILL_ACTIVE (259) which is indistinguishable from a process that
    // genuinely exited with code 259.
    const DWORD state = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return state == WAIT_TIMEOUT;   // not signalled => still running
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;          // EPERM etc: exists, just not ours
#endif
}


// Block until `pid` is gone, or until `timeoutMs` has elapsed. Returns true
// iff the process is no longer running.
//
// "Did it actually exit?" is the only honest answer available to a shutdown
// RPC that produced no reply (see RpcClient::shutdown), and a *positive*
// answer is what separates "the daemon died before it could finish speaking"
// -- a success -- from "the daemon is wedged and said nothing" -- a failure.
//
// Polling rather than waitpid(): the daemon is not our child. It was detached,
// or started by an entirely different shell, so kill(pid, 0) is the only
// liveness question we are allowed to ask about it.
inline bool waitForProcessExit(long long pid, int timeoutMs, int pollMs = 50)
{
    if (pid <= 0) return false;
    if (pollMs <= 0) pollMs = 1;

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (!processAlive(pid)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }
}

}  // namespace logosctl

#endif  // LOGOS_PROCESS_UTIL_H
