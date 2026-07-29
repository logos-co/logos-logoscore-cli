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

std::size_t countLogFiles(const std::string& dir, const std::string& stem)
{
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().rfind(stem, 0) == 0) ++n;
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

    const std::size_t files = countLogFiles(dir, "daemon");
    EXPECT_GT(files, 1u) << "expected the log to have rotated";
    EXPECT_LE(files, 3u) << "retention should cap the number of files kept";

    for (const auto& e : fs::directory_iterator(dir)) {
        // A little slack: the cap is checked per write, so the file can pass
        // it by at most the size of one line.
        EXPECT_LT(fs::file_size(e.path()), 2u * 1024 * 1024)
            << e.path().filename().string() << " ignored the size cap";
    }
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
