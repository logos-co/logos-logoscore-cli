#include "log_sink.h"
#include "../platform_compat.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <cerrno>
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// pipe(2). Windows' CRT spells it _pipe and wants a size and a text mode;
// _O_BINARY because this carries arbitrary bytes and CRLF translation would
// corrupt them (and duplicate every newline the reader splits on).
int makePipe(int fds[2])
{
#ifdef _WIN32
    return ::_pipe(fds, 64 * 1024, _O_BINARY);
#else
    return ::pipe(fds);
#endif
}

// yyyymmdd_HHMMSS in local time, matching basecamp's session stamp so logs
// from the two frontends sort and read the same way.
std::string sessionStamp()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    logosctl::localtimeR(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

struct NameParts { std::string stem, ext; };

NameParts split(const std::string& file)
{
    const fs::path p(file);
    return { p.stem().string(), p.extension().string() };
}

// Keep at most `keep` log files in `dir`, deleting the oldest first.
//
// spdlog's own max_files only prunes within one sink's rotation set, and each
// daemon start opens a new stamped base name -- so without this a daemon
// restarted a hundred times would leave a hundred logs behind. basecamp has
// exactly that problem; retention here is meant to actually bound the
// directory, so it prunes across sessions too.
void pruneOldLogs(const std::string& dir, const NameParts& n, std::size_t keep)
{
    struct Entry { fs::path path; fs::file_time_type when; };
    std::vector<Entry> found;

    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string name = e.path().filename().string();
        // Only our own stamped files: "<stem>_..." . The stable symlink is
        // "<stem><ext>" with no underscore, so it is never a candidate, and
        // neither is anything else that happens to share the directory.
        if (name.rfind(n.stem + "_", 0) != 0) continue;
        if (!n.ext.empty() && e.path().extension().string() != n.ext) continue;
        std::error_code tec;
        const auto when = fs::last_write_time(e.path(), tec);
        if (tec) continue;
        found.push_back({ e.path(), when });
    }

    if (found.size() <= keep) return;
    std::sort(found.begin(), found.end(),
              [](const Entry& a, const Entry& b) { return a.when < b.when; });
    for (std::size_t i = 0; i + keep < found.size(); ++i) {
        std::error_code rec;
        fs::remove(found[i].path, rec);
    }
}

}  // namespace

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

    const NameParts parts = split(opts.file);

    // Make room before opening the new file, so the count after startup is the
    // configured maximum rather than one over it.
    if (opts.maxFiles > 0)
        pruneOldLogs(opts.dir, parts, opts.maxFiles - 1);

    const std::string stamped = parts.stem + "_" + sessionStamp() + parts.ext;
    m_path     = (fs::path(opts.dir) / stamped).string();
    m_linkPath = stablePath(opts.dir, opts.file);
    m_console  = opts.console;

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
        m_linkPath.clear();
        return false;
    }

    // Point the stable name at this session's file. Callers -- and anyone
    // running `tail -F` -- get one path that is always current, instead of
    // having to work out the stamp. Best-effort: a filesystem without symlinks
    // costs the convenience, not the logging.
    if (!m_linkPath.empty()) {
        std::error_code lec;
        fs::remove(m_linkPath, lec);
        fs::create_symlink(stamped, m_linkPath, lec);
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
    if (makePipe(fds) != 0) {
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

#ifdef _WIN32
    // The dup2 above rebinds only the CRT's fd table. A MODULE SUBPROCESS does
    // not inherit that table: CreateProcess passes the STARTUPINFO std handles,
    // which default to the parent's STD_OUTPUT_HANDLE / STD_ERROR_HANDLE --
    // still the console. Capturing module-host output is the entire reason this
    // sink is pipe-based rather than a file redirect, so leaving the Win32 std
    // handles alone would silently defeat it: the daemon's own lines would be
    // logged and every logos_host line would keep going to the terminal.
    //
    // Take the handles from fd 1 / fd 2 AFTER the dup2, not from fds[1]: _dup2
    // duplicates the underlying OS handle into the target slot, so these stay
    // valid after the ::close(fds[1]) below, whereas fds[1]'s handle does not.
    m_originalStdoutHandle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    m_originalStderrHandle = ::GetStdHandle(STD_ERROR_HANDLE);
    ::SetStdHandle(STD_OUTPUT_HANDLE,
                   reinterpret_cast<HANDLE>(::_get_osfhandle(_fileno(stdout))));
    ::SetStdHandle(STD_ERROR_HANDLE,
                   reinterpret_cast<HANDLE>(::_get_osfhandle(_fileno(stderr))));
#endif

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
        // `auto`, not ssize_t: the CRT's read() returns int and takes an
        // unsigned int count, so the POSIX signature cannot be spelled here.
        const auto n = ::read(m_readFd, buf.data(),
                              static_cast<unsigned int>(buf.size()));
        if (n > 0) {
            // Mirror first: if the process dies mid-write, the terminal has
            // already seen it.
            if (m_console && m_originalStdout >= 0) {
                const auto ignored = ::write(m_originalStdout, buf.data(),
                                             static_cast<unsigned int>(n));
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

#ifdef _WIN32
    // Put the Win32 std handles back in the same breath as the fds. The
    // handles we installed in start() are the ones fd 1 / fd 2 own, so the
    // dup2 above has just closed them -- leaving STD_OUTPUT_HANDLE dangling
    // for anything (a child spawned during shutdown) that reads it.
    if (m_originalStdoutHandle) {
        ::SetStdHandle(STD_OUTPUT_HANDLE, m_originalStdoutHandle);
        m_originalStdoutHandle = nullptr;
    }
    if (m_originalStderrHandle) {
        ::SetStdHandle(STD_ERROR_HANDLE, m_originalStderrHandle);
        m_originalStderrHandle = nullptr;
    }
#endif

    if (m_readerThread.joinable())
        m_readerThread.join();

    if (m_originalStdout >= 0) { ::close(m_originalStdout); m_originalStdout = -1; }
    if (m_originalStderr >= 0) { ::close(m_originalStderr); m_originalStderr = -1; }
    if (m_readFd >= 0)         { ::close(m_readFd);         m_readFd = -1; }

    m_logger.reset();
    m_path.clear();
    m_linkPath.clear();
    m_started = false;
}

std::string LogSink::stablePath(const std::string& dir, const std::string& file)
{
    return (fs::path(dir) / file).string();
}

std::string LogSink::currentFile() const
{
    return m_path;
}
