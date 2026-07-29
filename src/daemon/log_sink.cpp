#include "log_sink.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

LogSink& LogSink::instance()
{
    static LogSink s;
    return s;
}

LogSink::~LogSink()
{
    stop();
}

bool LogSink::start(const Options& opts)
{
    if (m_started)
        return true;
    if (!opts.enabled)
        return true;   // disabled is a valid configuration, not a failure

    std::error_code ec;
    fs::create_directories(opts.dir, ec);
    if (ec)
        return false;

    m_path = (fs::path(opts.dir) / opts.file).string();
    m_console = opts.console;

    try {
        // maxSizeMb == 0 means "never rotate": spdlog has no such mode, so
        // approximate it with a cap large enough to be unreachable in practice
        // rather than silently imposing a limit the operator declined.
        const std::size_t maxBytes = opts.maxSizeMb == 0
            ? std::size_t(1) << 40
            : opts.maxSizeMb * 1024ull * 1024ull;
        // maxFiles is the total kept; spdlog counts rotated files *besides*
        // the live one, so subtract it.
        const std::size_t rotated = opts.maxFiles > 1 ? opts.maxFiles - 1 : 0;

        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            m_path, maxBytes, rotated);
        // Lines arriving from the pipe already carry liblogos's timestamp and
        // level; stamping them again would double every prefix.
        sink->set_pattern("%v");
        auto logger = std::make_shared<spdlog::logger>("logosctl-session", sink);
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);
        m_logger = logger;
    } catch (const std::exception&) {
        m_path.clear();
        return false;
    }

    auto cleanup = [this]() {
        if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
        if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }
        m_logger.reset();
        m_path.clear();
    };

    m_originalStdout = ::dup(fileno(stdout));
    m_originalStderr = ::dup(fileno(stderr));
    if (m_originalStdout < 0 || m_originalStderr < 0) {
        cleanup();
        return false;
    }

    int fds[2];
    if (::pipe(fds) != 0) {
        cleanup();
        return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);

    if (::dup2(fds[1], fileno(stdout)) == -1 ||
        ::dup2(fds[1], fileno(stderr)) == -1) {
        ::close(fds[0]);
        ::close(fds[1]);
        cleanup();
        return false;
    }

    // Line-buffer so a line reaches the reader as soon as it is complete,
    // rather than sitting in a 4K buffer until the daemon happens to fill it.
    // Without this a crash would lose the last, most interesting output.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    ::close(fds[1]);
    m_readFd = fds[0];

    m_running = true;
    m_readerThread = std::thread(&LogSink::readerLoop, this);
    m_started = true;
    return true;
}

void LogSink::readerLoop()
{
    auto logger = std::static_pointer_cast<spdlog::logger>(m_logger);
    std::string pending;
    std::vector<char> buf(4096);

    for (;;) {
        const ssize_t n = ::read(m_readFd, buf.data(), buf.size());
        if (n > 0) {
            // Mirror first: if the process dies mid-write, the terminal has
            // already seen it.
            if (m_console && m_originalStdout >= 0) {
                ssize_t ignored = ::write(m_originalStdout, buf.data(),
                                          static_cast<std::size_t>(n));
                (void)ignored;
            }
            pending.append(buf.data(), static_cast<std::size_t>(n));

            // Split on newlines; the sink appends its own, so hand it bare
            // lines and hold any partial tail until the rest arrives.
            std::size_t start = 0;
            for (std::size_t i = 0; i < pending.size(); ++i) {
                if (pending[i] != '\n') continue;
                if (logger)
                    logger->info(pending.substr(start, i - start));
                start = i + 1;
            }
            pending.erase(0, start);
            // A producer that never emits a newline would otherwise grow this
            // without bound; flush it as its own line well before that.
            if (pending.size() > 64 * 1024) {
                if (logger) logger->info(pending);
                pending.clear();
            }
        } else if (n == 0) {
            break;                        // write ends closed
        } else if (errno != EINTR) {
            break;
        }
    }

    if (!pending.empty() && logger)
        logger->info(pending);
    if (logger)
        logger->flush();
}

void LogSink::stop()
{
    if (!m_started)
        return;
    m_running = false;

    std::fflush(stdout);
    std::fflush(stderr);

    // dup2 back closes the pipe's write end held by stdout/stderr; once both
    // are restored the reader sees EOF and returns. The duplicated originals
    // stay open until the reader has finished, because readerLoop may still be
    // mirroring to m_originalStdout and closing it here would race fd reuse.
    if (m_originalStdout >= 0) ::dup2(m_originalStdout, fileno(stdout));
    if (m_originalStderr >= 0) ::dup2(m_originalStderr, fileno(stderr));

    if (m_readerThread.joinable())
        m_readerThread.join();

    if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
    if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }
    if (m_readFd >= 0)         { ::close(m_readFd);         m_readFd = -1; }

    m_logger.reset();
    m_path.clear();
    m_started = false;
}

std::string LogSink::currentFile() const
{
    return m_path;
}
