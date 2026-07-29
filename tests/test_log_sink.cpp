#include <gtest/gtest.h>

#include "daemon/log_sink.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string uniqueDir(const char* tag)
{
    const char* t = std::getenv("TMPDIR");
    std::string base = t && *t ? t : "/tmp";
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/logosctl_" + tag + "_" + std::to_string(::getpid());
}

// Counts real log files only. The stable name is a symlink into this same
// directory, so counting it would inflate every retention assertion by one.
std::size_t countLogFiles(const std::string& dir, const std::string& stem)
{
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_symlink()) continue;
        if (e.path().filename().string().rfind(stem, 0) == 0) ++n;
    }
    return n;
}

} // namespace

class LogSinkTest : public ::testing::Test {
protected:
    std::string dir;
    void SetUp() override {
        dir = uniqueDir("logsink");
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override {
        LogSink::instance().stop();
        fs::remove_all(dir);
    }
};

// Anything written to stdout/stderr -- by the daemon or by a module subprocess
// that inherited the descriptors -- has to reach the file. That is the whole
// reason capture is pipe-based rather than a plain redirect.
TEST_F(LogSinkTest, CapturesStdoutAndStderr)
{
    LogSink::Options o;
    o.dir = dir;
    o.console = false;   // don't spray the test runner's output
    ASSERT_TRUE(LogSink::instance().start(o));

    std::printf("hello-from-stdout\n");
    std::fprintf(stderr, "hello-from-stderr\n");
    std::fflush(stdout);
    std::fflush(stderr);

    LogSink::instance().stop();   // drains the reader before we read the file

    std::ifstream ifs(dir + "/daemon.log");
    ASSERT_TRUE(ifs.good());
    const std::string body((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("hello-from-stdout"), std::string::npos);
    EXPECT_NE(body.find("hello-from-stderr"), std::string::npos);
}

// Lines arriving from the pipe already carry liblogos's own timestamp and
// level. The sink must not stamp them a second time.
TEST_F(LogSinkTest, DoesNotRestampAlreadyFormattedLines)
{
    LogSink::Options o;
    o.dir = dir;
    o.console = false;
    ASSERT_TRUE(LogSink::instance().start(o));

    std::printf("[2026-07-29 16:38:32.715] [info] [logos] already-formatted\n");
    std::fflush(stdout);
    LogSink::instance().stop();

    std::ifstream ifs(dir + "/daemon.log");
    std::string line;
    std::getline(ifs, line);
    EXPECT_EQ(line, "[2026-07-29 16:38:32.715] [info] [logos] already-formatted");
}

// A long-lived daemon must not grow its log without bound, and must not keep
// every rotation forever either.
TEST_F(LogSinkTest, RotatesAtTheSizeCapAndKeepsOnlyMaxFiles)
{
    LogSink::Options o;
    o.dir = dir;
    o.console = false;
    o.maxSizeMb = 1;     // smallest cap the schema allows
    o.maxFiles = 3;      // live + 2 rotated
    ASSERT_TRUE(LogSink::instance().start(o));

    // Comfortably past 3 MB, so rotation must have happened more than
    // maxFiles times and the oldest files must have been dropped.
    const std::string chunk(512, 'x');
    for (int i = 0; i < 8000; ++i)
        std::printf("%06d %s\n", i, chunk.c_str());
    std::fflush(stdout);

    LogSink::instance().stop();

    const std::size_t files = countLogFiles(dir, "daemon_");
    EXPECT_GT(files, 1u) << "expected the log to have rotated";
    EXPECT_LE(files, 3u) << "retention should cap the number of files kept";

    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_symlink()) continue;
        // A little slack: the cap is checked per write, so the file can pass
        // it by at most the size of one line.
        EXPECT_LT(fs::file_size(e.path()), 2u * 1024 * 1024)
            << e.path().filename().string() << " ignored the size cap";
    }
}

// The real file carries a start-time stamp, and the configured name survives
// as a symlink to it -- so `tail -F logs/daemon.log` follows the current
// session without anyone having to work out the stamp.
TEST_F(LogSinkTest, StampsTheFileAndLinksTheStableName)
{
    LogSink::Options o;
    o.dir = dir;
    o.console = false;
    ASSERT_TRUE(LogSink::instance().start(o));

    const std::string real = LogSink::instance().currentFile();
    const std::string link = LogSink::stablePath(dir, o.file);
    std::printf("marker-line\n");
    std::fflush(stdout);
    LogSink::instance().stop();

    // daemon_YYYYmmdd_HHMMSS.log -- stamped, and not the bare name.
    const std::string base = fs::path(real).filename().string();
    EXPECT_NE(base, "daemon.log");
    EXPECT_EQ(base.rfind("daemon_", 0), 0u);
    EXPECT_EQ(fs::path(real).extension().string(), ".log");
    EXPECT_EQ(base.size(), std::string("daemon_20260729_163832.log").size());

    ASSERT_TRUE(fs::is_symlink(link));
    std::ifstream viaLink(link);
    const std::string body((std::istreambuf_iterator<char>(viaLink)),
                            std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("marker-line"), std::string::npos)
        << "the stable name must resolve to this session's file";
}

// spdlog's own max_files only prunes one sink's rotation set, and every start
// opens a new stamped name -- so retention has to prune across sessions or a
// repeatedly restarted daemon fills the disk. basecamp has that bug; this
// must not.
TEST_F(LogSinkTest, RetentionPrunesAcrossSessions)
{
    for (int session = 0; session < 5; ++session) {
        LogSink::Options o;
        o.dir = dir;
        o.console = false;
        o.maxFiles = 2;
        ASSERT_TRUE(LogSink::instance().start(o));
        std::printf("session %d\n", session);
        std::fflush(stdout);
        LogSink::instance().stop();

        // Stamps have one-second resolution, so without this two sessions
        // would collide on the same filename and the test would prove nothing.
        if (session < 4) ::sleep(1);
    }

    EXPECT_EQ(countLogFiles(dir, "daemon_"), 2u)
        << "five sessions with max_files: 2 should leave two log files";
    // The stable link must survive pruning and still resolve.
    const std::string link = LogSink::stablePath(dir, "daemon.log");
    EXPECT_TRUE(fs::is_symlink(link));
    EXPECT_TRUE(fs::exists(fs::read_symlink(link).is_absolute()
                               ? fs::read_symlink(link)
                               : fs::path(dir) / fs::read_symlink(link)));
}

// Disabled logging is a valid configuration, not an error, and must leave
// stdout alone.
TEST_F(LogSinkTest, DisabledWritesNothingAndSucceeds)
{
    LogSink::Options o;
    o.dir = dir;
    o.enabled = false;
    EXPECT_TRUE(LogSink::instance().start(o));
    EXPECT_TRUE(LogSink::instance().currentFile().empty());
    EXPECT_FALSE(fs::exists(dir + "/daemon.log"));
}
