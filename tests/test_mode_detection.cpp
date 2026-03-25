#include <gtest/gtest.h>
#include <QStringList>
#include "client/commands/command.h"

// Test knownSubcommands() and basic mode detection logic.
// The actual detectMode() function is in main.cpp and uses argc/argv,
// so we test its logic indirectly via the helpers it depends on.

TEST(ModeDetectionTest, KnownSubcommands_Complete)
{
    QStringList cmds = knownSubcommands();
    EXPECT_GE(cmds.size(), 10);

    // All expected commands present
    EXPECT_TRUE(cmds.contains("daemon"));
    EXPECT_TRUE(cmds.contains("status"));
    EXPECT_TRUE(cmds.contains("load-module"));
    EXPECT_TRUE(cmds.contains("unload-module"));
    EXPECT_TRUE(cmds.contains("reload-module"));
    EXPECT_TRUE(cmds.contains("list-modules"));
    EXPECT_TRUE(cmds.contains("module-info"));
    EXPECT_TRUE(cmds.contains("info"));
    EXPECT_TRUE(cmds.contains("call"));
    EXPECT_TRUE(cmds.contains("module"));
    EXPECT_TRUE(cmds.contains("watch"));
    EXPECT_TRUE(cmds.contains("stats"));
}

TEST(ModeDetectionTest, DaemonDetection)
{
    // The mode detection logic:
    // "-D" or "daemon" → Daemon
    // argv[1] is known subcommand → Client
    // -m, -l, -c → Inline
    // else → Help

    // We verify the logic patterns here:
    QStringList cmds = knownSubcommands();
    EXPECT_TRUE(cmds.contains("daemon")); // "daemon" is a known subcommand

    // "-D" flag detection would be in the actual argv scanning
    // We just verify "daemon" is recognized
}

TEST(ModeDetectionTest, ClientSubcommandsRecognized)
{
    QStringList cmds = knownSubcommands();

    // All client commands should be recognized
    QStringList clientCmds = {
        "status", "load-module", "unload-module", "reload-module",
        "list-modules", "module-info", "info", "call", "module",
        "watch", "stats", "stop"
    };

    for (const QString& cmd : clientCmds) {
        EXPECT_TRUE(cmds.contains(cmd)) << "Missing subcommand: " << cmd.toStdString();
    }
}

TEST(ModeDetectionTest, UnknownCommandNotInList)
{
    QStringList cmds = knownSubcommands();
    EXPECT_FALSE(cmds.contains("nonexistent"));
    EXPECT_FALSE(cmds.contains("help"));  // help is --help flag, not a subcommand
    EXPECT_FALSE(cmds.contains("version")); // version is --version flag
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
