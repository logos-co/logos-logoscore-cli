#ifndef LOG_SINK_H
#define LOG_SINK_H

#include <atomic>
#include <memory>
#include <string>
#include <thread>

// Captures everything the daemon and its module subprocesses write to
// stdout/stderr into a rotating log file.
//
// Two decisions worth knowing:
//
//   * Capture is **pipe-based**, not a plain file redirect. Module hosts are
//     separate processes that inherit fd 1 and 2, so redirecting to a file
//     would catch their output but leave rotation impossible — renaming a file
//     out from under a child that holds it open just keeps writing to the old
//     inode. A pipe puts one reader in charge of the file, so rotation is
//     safe, and child output still lands in it.
//
//   * The writer is **spdlog's rotating sink**, which already implements the
//     size cap and the keep-N-files retention. Lines arriving from the pipe
//     are already formatted (they carry liblogos's own timestamps and levels),
//     so the sink uses a raw "%v" pattern rather than stamping them twice.
//
// Nothing here is required for the daemon to run: if the log file cannot be
// opened, start() reports it and the daemon carries on writing to whatever
// stdout it had.
class LogSink {
public:
    struct Options {
        bool        enabled  = true;
        std::string dir;                 // resolved logs directory
        // Names the log. The real file gets a start-time stamp inserted --
        // "daemon.log" becomes "daemon_20260729_163832.log" -- and this exact
        // name survives as a symlink to whichever file is current, so
        // `tail -F logs/daemon.log` keeps working across restarts.
        std::string file     = "daemon.log";
        // Rotate once the file passes this size, keeping `maxFiles` of them
        // (the live one plus maxFiles-1 older). 0 disables rotation.
        std::size_t maxSizeMb = 10;
        std::size_t maxFiles  = 5;
        // Mirror to the terminal as well. Pointless once detached, where the
        // original stdout is /dev/null, so the daemon turns it off there.
        bool console = true;
    };

    static LogSink& instance();

    // Returns false if file logging could not be started; the caller should
    // warn and continue rather than abort.
    bool start(const Options& opts);

    // Restore the original stdout/stderr and drain the reader. Safe to call
    // when never started.
    void stop();

    // Path of the live, timestamped log file, or empty when not started.
    std::string currentFile() const;

    // The stable symlink that points at it (<dir>/<file>). Safe to print
    // before the daemon has started, which is why --detach reports this
    // rather than a filename it would have to guess the stamp for.
    static std::string stablePath(const std::string& dir, const std::string& file);

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

private:
    LogSink() = default;
    ~LogSink();

    void readerLoop();

    struct Impl;
    std::shared_ptr<void> m_logger;   // spdlog::logger, type-erased to keep
                                      // spdlog out of this header
    std::string m_path;      // timestamped file actually written
    std::string m_linkPath;  // stable symlink pointing at it

    int m_readFd         = -1;
    int m_originalStdout = -1;
    int m_originalStderr = -1;

    // Windows only: the STD_OUTPUT_HANDLE / STD_ERROR_HANDLE that were in
    // place before start() repointed them at the pipe, so stop() can put them
    // back. Held as void* to keep windows.h out of this header; they are
    // HANDLEs. Unused on POSIX, where dup2 alone is the whole story.
    void* m_originalStdoutHandle = nullptr;
    void* m_originalStderrHandle = nullptr;

    std::thread       m_readerThread;
    std::atomic<bool> m_running{false};
    bool              m_started = false;
    bool              m_console = true;
};

#endif // LOG_SINK_H
