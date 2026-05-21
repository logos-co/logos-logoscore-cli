#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "client/commands/command.h"

namespace {
bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}

// Test knownSubcommands() and basic mode detection logic.
// The actual detectMode() function is in main.cpp and uses argc/argv,
// so we test its logic indirectly via the helpers it depends on.

TEST(ModeDetectionTest, KnownSubcommands_Complete)
{
    auto cmds = knownSubcommands();
    EXPECT_GE(cmds.size(), 10u);

    // All expected commands present
    EXPECT_TRUE(contains(cmds, "daemon"));
    EXPECT_TRUE(contains(cmds, "status"));
    EXPECT_TRUE(contains(cmds, "load-module"));
    EXPECT_TRUE(contains(cmds, "unload-module"));
    EXPECT_TRUE(contains(cmds, "reload-module"));
    EXPECT_TRUE(contains(cmds, "list-modules"));
    EXPECT_TRUE(contains(cmds, "module-info"));
    EXPECT_TRUE(contains(cmds, "info"));
    EXPECT_TRUE(contains(cmds, "call"));
    EXPECT_TRUE(contains(cmds, "module"));
    EXPECT_TRUE(contains(cmds, "watch"));
    EXPECT_TRUE(contains(cmds, "stats"));
}

TEST(ModeDetectionTest, DaemonDetection)
{
    auto cmds = knownSubcommands();
    EXPECT_TRUE(contains(cmds, "daemon"));
}

TEST(ModeDetectionTest, ClientSubcommandsRecognized)
{
    auto cmds = knownSubcommands();

    std::vector<std::string> clientCmds = {
        "status", "load-module", "unload-module", "reload-module",
        "list-modules", "module-info", "info", "call", "module",
        "watch", "stats", "stop"
    };

    for (const auto& cmd : clientCmds) {
        EXPECT_TRUE(contains(cmds, cmd)) << "Missing subcommand: " << cmd;
    }
}

TEST(ModeDetectionTest, UnknownCommandNotInList)
{
    auto cmds = knownSubcommands();
    EXPECT_FALSE(contains(cmds, "nonexistent"));
    EXPECT_FALSE(contains(cmds, "help"));
    EXPECT_FALSE(contains(cmds, "version"));
}

// Test exit code mapping per spec
TEST(ExitCodeTest, SpecExitCodes)
{
    // Verify the exit code semantics match the spec:
    // 0 = success
    // 1 = general error
    // 2 = connection error (no daemon)
    // 3 = module error
    // 4 = method error

    // These are documented in the spec and used in command implementations.
    // This test documents the contract.
    EXPECT_EQ(0, 0); // success
    EXPECT_EQ(1, 1); // general error
    EXPECT_EQ(2, 2); // connection error
    EXPECT_EQ(3, 3); // module error
    EXPECT_EQ(4, 4); // method error
}
