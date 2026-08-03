#include <gtest/gtest.h>

#include "paths.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

// relaunchPath answers "what do I exec to run myself again", which is NOT the
// same as "where is my binary".
//
// A portable Linux bundle ships `bin/logosctl` as a launcher script beside a
// hidden `bin/.logosctl.elf`, and only the launcher can start the program --
// the ELF's PT_INTERP points at a loader that does not exist on the host, so
// exec'ing it returns ENOENT. --detach re-exec'd the ELF and the daemon died
// with status 127 and no output whatsoever: no log file, no diagnostic, just
// "daemon exited during startup".
class RelaunchPathTest : public ::testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        dir = fs::temp_directory_path() /
              ("logosctl_relaunch_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    void TearDown() override { fs::remove_all(dir); }

    fs::path makeExecutable(const std::string& name) {
        const fs::path p = dir / name;
        std::ofstream(p) << "#!/bin/sh\nexit 0\n";
        fs::permissions(p, fs::perms::owner_all | fs::perms::group_exec
                                                | fs::perms::others_exec);
        return p;
    }
};

TEST_F(RelaunchPathTest, PrefersArgv0OverTheResolvedBinary)
{
    const fs::path launcher = makeExecutable("logosctl");
    EXPECT_EQ(paths::relaunchPath(launcher.c_str()), launcher.string())
        << "argv[0] is the launcher the caller actually ran; the resolved "
           "binary may be an ELF that cannot be exec'd on its own";
}

// The daemon it spawns may run from anywhere, so a relative argv[0] has to be
// pinned to a path before it is handed over.
TEST_F(RelaunchPathTest, MakesARelativeArgv0Absolute)
{
    const fs::path launcher = makeExecutable("logosctl");
    const fs::path cwd = fs::current_path();
    fs::current_path(dir);
    const std::string got = paths::relaunchPath("./logosctl");
    fs::current_path(cwd);

    EXPECT_TRUE(fs::path(got).is_absolute()) << got;
    EXPECT_EQ(fs::path(got).filename(), "logosctl");
    EXPECT_EQ(fs::canonical(got), fs::canonical(launcher));
}

TEST_F(RelaunchPathTest, ResolvesABareNameThroughPath)
{
    makeExecutable("logosctl");
    const char* old = std::getenv("PATH");
    const std::string saved = old ? old : "";
    setenv("PATH", (dir.string() + ":" + saved).c_str(), 1);
    const std::string got = paths::relaunchPath("logosctl");
    setenv("PATH", saved.c_str(), 1);

    EXPECT_EQ(got, (dir / "logosctl").string())
        << "invoked by name, we must find the same file the shell found";
}

// A name that isn't on PATH, an argv[0] that doesn't exist, or no argv[0] at
// all must not yield a bogus path — fall back to the binary we know about.
TEST_F(RelaunchPathTest, FallsBackWhenArgv0IsUseless)
{
    EXPECT_EQ(paths::relaunchPath(nullptr), paths::executablePath());
    EXPECT_EQ(paths::relaunchPath(""), paths::executablePath());
    EXPECT_EQ(paths::relaunchPath((dir / "nope").c_str()),
              paths::executablePath());
}

// A directory is not something you can exec, and neither is a plain file.
// The one that actually bit: a portable Linux bundle. Both argv[0] and
// /proc/self/exe name `bin/.logosctl.elf`, which cannot be exec'd because its
// PT_INTERP is absent — the launcher beside it is the only way in.
TEST_F(RelaunchPathTest, MapsAHiddenBundleElfBackToItsLauncher)
{
    const fs::path launcher = makeExecutable("logosctl");
    const fs::path hidden   = makeExecutable(".logosctl.elf");

    EXPECT_EQ(paths::relaunchPath(hidden.c_str()), launcher.string())
        << "exec'ing the hidden ELF fails with ENOENT; the launcher is what "
           "the bundle provides to start it";
}

// Only when the launcher is really there. A stray `.foo.elf` with no sibling
// must be left alone rather than rewritten into a path that does not exist.
TEST_F(RelaunchPathTest, LeavesAHiddenElfAloneWithoutALauncher)
{
    const fs::path hidden = makeExecutable(".orphan.elf");
    EXPECT_EQ(paths::relaunchPath(hidden.c_str()), hidden.string());
}

// A normal binary that merely ends in .elf is not the bundle convention —
// the convention is a LEADING dot plus a sibling of the undotted name.
TEST_F(RelaunchPathTest, DoesNotRewriteAnOrdinaryDotElfName)
{
    makeExecutable("logosctl");
    const fs::path plain = makeExecutable("logosctl.elf");
    EXPECT_EQ(paths::relaunchPath(plain.c_str()), plain.string());
}

TEST_F(RelaunchPathTest, IgnoresNonExecutableCandidates)
{
    const fs::path notExec = dir / "logosctl";
    std::ofstream(notExec) << "not a program\n";
    fs::permissions(notExec, fs::perms::owner_read | fs::perms::owner_write);
    EXPECT_EQ(paths::relaunchPath(notExec.c_str()), paths::executablePath());

    fs::create_directories(dir / "adir");
    EXPECT_EQ(paths::relaunchPath((dir / "adir").c_str()),
              paths::executablePath());
}
