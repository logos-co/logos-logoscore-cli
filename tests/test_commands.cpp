#include <gtest/gtest.h>

#include <filesystem>
#include <logos_json.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"

// Mock client for testing commands without a real daemon
class MockClient : public Client {
public:
    // Control mock behavior
    bool shouldConnect = true;
    std::string connectError = "No running logosctl daemon. Start one with: logosctl -D";
    LogosMap  loadModuleResult;
    LogosMap  unloadModuleResult;
    LogosMap  reloadModuleResult;
    LogosList listModulesResult;
    LogosMap  statusResult;
    LogosMap  moduleInfoResult;
    LogosList moduleStatsResult;
    LogosMap  callMethodResult;
    LogosMap  shutdownResult;
    LogosMap  refreshModulesResult;
    LogosMap  planPackageResult;
    LogosMap  applyPackageResult;
    LogosMap  downloadResult;

    // Track calls
    bool shutdownCalled = false;
    bool refreshModulesCalled = false;
    bool applyPackageCalled = false;
    std::string lastPackageOp;
    LogosList   lastPackageNames;
    LogosMap    lastPackageOpts;
    std::string lastDownloadName;
    LogosMap    lastDownloadOpts;
    std::string lastLoadedModule;
    std::string lastUnloadedModule;
    // Defaults to true so a test that never sets --no-dependents still sees
    // the production default rather than a value-initialised false.
    bool lastUnloadWithDependents = true;
    std::string lastReloadedModule;
    std::string lastInfoModule;
    std::string lastCallModule;
    std::string lastCallMethod;
    LogosList   lastCallArgs;
    std::string lastListFilter;
    std::string lastWatchModule;
    std::string lastWatchEventName;
    bool watchShouldSucceed = false;

    bool connect() override {
        m_connected = shouldConnect;
        m_lastError = shouldConnect ? "" : connectError;
        return shouldConnect;
    }

    bool isConnected() const override { return m_connected; }
    std::string lastError() const override { return m_lastError; }

    LogosMap loadModule(const std::string& name) override {
        lastLoadedModule = name;
        return loadModuleResult;
    }

    LogosMap unloadModule(const std::string& name, bool withDependents) override {
        lastUnloadedModule = name;
        lastUnloadWithDependents = withDependents;
        return unloadModuleResult;
    }

    LogosMap refreshModules() override {
        refreshModulesCalled = true;
        return refreshModulesResult;
    }

    LogosMap planPackageOperation(const std::string& op, const LogosList& names,
                                  const LogosMap& opts) override {
        lastPackageOp = op;
        lastPackageNames = names;
        lastPackageOpts = opts;
        return planPackageResult;
    }

    LogosMap applyPackageOperation(const std::string& op, const LogosList& names,
                                   const LogosMap& opts) override {
        lastPackageOp = op;
        lastPackageNames = names;
        lastPackageOpts = opts;
        applyPackageCalled = true;
        return applyPackageResult;
    }

    LogosMap downloadPackage(const std::string& name, const LogosMap& opts) override {
        lastDownloadName = name;
        lastDownloadOpts = opts;
        return downloadResult;
    }

    LogosMap reloadModule(const std::string& name) override {
        lastReloadedModule = name;
        return reloadModuleResult;
    }

    LogosList listModules(const std::string& filter) override {
        lastListFilter = filter;
        return listModulesResult;
    }

    LogosMap getStatus() override { return statusResult; }

    LogosMap getModuleInfo(const std::string& name) override {
        lastInfoModule = name;
        return moduleInfoResult;
    }

    LogosList getModuleStats() override { return moduleStatsResult; }

    LogosMap callModuleMethod(const std::string& module, const std::string& method,
                               const LogosList& args) override {
        lastCallModule = module;
        lastCallMethod = method;
        lastCallArgs   = args;
        return callMethodResult;
    }

    LogosMap shutdown() override {
        shutdownCalled = true;
        return shutdownResult;
    }

    bool watchModuleEvents(const std::string& module, const std::string& eventName,
                            std::function<void(const LogosMap&)> callback) override {
        (void)callback;
        lastWatchModule    = module;
        lastWatchEventName = eventName;
        return m_connected && watchShouldSucceed;
    }

private:
    bool m_connected = false;
    std::string m_lastError;
};

class CommandTest : public ::testing::Test {
protected:
    MockClient mockClient;
    Output output{true}; // Force JSON mode for testable output

    void SetUp() override {
        mockClient.shouldConnect = true;
    }

    std::string captureOutput(std::function<void()> fn) {
        std::stringstream buffer;
        auto oldBuf = std::cout.rdbuf(buffer.rdbuf());
        fn();
        std::cout.rdbuf(oldBuf);
        return buffer.str();
    }

    nlohmann::json parseJson(const std::string& s) {
        return nlohmann::json::parse(s);
    }
};

// ── createCommand ────────────────────────────────────────────────────────────

TEST_F(CommandTest, CreateCommand_KnownCommands)
{
    EXPECT_NE(createCommand("status",        mockClient, output), nullptr);
    EXPECT_NE(createCommand("load-module",   mockClient, output), nullptr);
    EXPECT_NE(createCommand("unload-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("reload-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("list-modules",  mockClient, output), nullptr);
    EXPECT_NE(createCommand("module-info",   mockClient, output), nullptr);
    EXPECT_NE(createCommand("info",          mockClient, output), nullptr);
    EXPECT_NE(createCommand("call",          mockClient, output), nullptr);
    EXPECT_NE(createCommand("module",        mockClient, output), nullptr);
    EXPECT_NE(createCommand("watch",         mockClient, output), nullptr);
    EXPECT_NE(createCommand("stats",         mockClient, output), nullptr);
    EXPECT_NE(createCommand("stop",          mockClient, output), nullptr);
    EXPECT_NE(createCommand("issue-token",   mockClient, output), nullptr);
    EXPECT_NE(createCommand("revoke-token",  mockClient, output), nullptr);
    EXPECT_NE(createCommand("list-tokens",   mockClient, output), nullptr);
}

TEST_F(CommandTest, CreateCommand_Unknown_ReturnsNull)
{
    EXPECT_EQ(createCommand("nonexistent", mockClient, output), nullptr);
}

// ── knownSubcommands ─────────────────────────────────────────────────────────

TEST_F(CommandTest, KnownSubcommands_ContainsExpected)
{
    auto cmds = knownSubcommands();
    auto has  = [&](const std::string& s) {
        return std::find(cmds.begin(), cmds.end(), s) != cmds.end();
    };
    EXPECT_TRUE(has("status"));
    EXPECT_TRUE(has("load-module"));
    EXPECT_TRUE(has("unload-module"));
    EXPECT_TRUE(has("reload-module"));
    EXPECT_TRUE(has("list-modules"));
    EXPECT_TRUE(has("module-info"));
    EXPECT_TRUE(has("info"));
    EXPECT_TRUE(has("call"));
    EXPECT_TRUE(has("watch"));
    EXPECT_TRUE(has("stats"));
    EXPECT_TRUE(has("stop"));
    EXPECT_TRUE(has("daemon"));
    EXPECT_TRUE(has("issue-token"));
    EXPECT_TRUE(has("revoke-token"));
    EXPECT_TRUE(has("list-tokens"));
}

// ── Connection Error Handling ────────────────────────────────────────────────

TEST_F(CommandTest, LoadModule_NoDaemon_ReturnsExit2)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("load-module", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 2);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

// ── load-module ──────────────────────────────────────────────────────────────

TEST_F(CommandTest, LoadModule_Success)
{
    mockClient.loadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}, {"version", "0.1.0"},
        {"dependencies_loaded", nlohmann::json::array({"store"})}
    };

    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastLoadedModule, "waku");

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
    EXPECT_EQ(doc["module"].get<std::string>(), "waku");
}

TEST_F(CommandTest, LoadModule_NotFound)
{
    mockClient.loadModuleResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_NOT_FOUND"},
        {"message", "Module 'nonexistent' not found."}
    };

    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"nonexistent"});
        EXPECT_EQ(exitCode, 3);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "MODULE_NOT_FOUND");
}

TEST_F(CommandTest, LoadModule_MissingArg)
{
    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "INVALID_ARGS");
}

// ── unload-module ────────────────────────────────────────────────────────────

TEST_F(CommandTest, UnloadModule_Success)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}
    };

    auto cmd = createCommand("unload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastUnloadedModule, "waku");
}

// The cascade is the default — a bare `unload-module X` must take X's
// dependents down with it, since leaving them bound to an unloaded provider
// is the more surprising outcome.
TEST_F(CommandTest, UnloadModule_CascadesToDependentsByDefault)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}
    };
    mockClient.lastUnloadWithDependents = false;  // prove the command sets it

    auto cmd = createCommand("unload-module", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"waku"}), 0);
    });

    EXPECT_TRUE(mockClient.lastUnloadWithDependents);
}

TEST_F(CommandTest, UnloadModule_NoDependentsOptsOutOfCascade)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"}
    };

    auto cmd = createCommand("unload-module", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"waku", "--no-dependents"}), 0);
    });

    EXPECT_EQ(mockClient.lastUnloadedModule, "waku");
    EXPECT_FALSE(mockClient.lastUnloadWithDependents);
}

// A cascade that quietly stops three other modules has to be reported.
TEST_F(CommandTest, UnloadModule_Human_ReportsUnloadedDependents)
{
    mockClient.unloadModuleResult = LogosMap{
        {"status", "ok"}, {"module", "waku"},
        {"dependents_unloaded", LogosList{"chat", "delivery"}}
    };

    output.setHumanMode(true);
    auto cmd = createCommand("unload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"waku"}), 0);
    });

    EXPECT_NE(out.find("chat"), std::string::npos);
    EXPECT_NE(out.find("delivery"), std::string::npos);
}

// ── reload-module ────────────────────────────────────────────────────────────

TEST_F(CommandTest, ReloadModule_Success)
{
    mockClient.reloadModuleResult = LogosMap{
        {"action", "reload"}, {"module", "chat"}, {"version", "0.2.0"},
        {"status", "loaded"}, {"pid", 51203}
    };

    auto cmd = createCommand("reload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastReloadedModule, "chat");
}

TEST_F(CommandTest, ReloadModule_Error)
{
    mockClient.reloadModuleResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_LOAD_FAILED"},
        {"message", "Module failed to start."}
    };

    auto cmd = createCommand("reload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 3);
    });
}

// ── list-modules ─────────────────────────────────────────────────────────────

TEST_F(CommandTest, ListModules_All)
{
    mockClient.listModulesResult = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"status", "loaded"}},
        LogosMap{{"name", "chat"}, {"status", "not_loaded"}}
    });

    auto cmd = createCommand("list-modules", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastListFilter, "all");
}

TEST_F(CommandTest, ListModules_LoadedFilter)
{
    mockClient.listModulesResult = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"status", "loaded"}}
    });

    auto cmd = createCommand("list-modules", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"--loaded"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastListFilter, "loaded");
}

// ── module-info / info ───────────────────────────────────────────────────────

TEST_F(CommandTest, ModuleInfo_Success)
{
    mockClient.moduleInfoResult = LogosMap{
        {"name", "chat"}, {"version", "0.2.0"}, {"status", "loaded"},
        {"pid", 23457}, {"uptime_seconds", 8040}
    };

    auto cmd = createCommand("module-info", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastInfoModule, "chat");
}

TEST_F(CommandTest, InfoAlias_SameAsModuleInfo)
{
    mockClient.moduleInfoResult = LogosMap{
        {"name", "chat"}, {"version", "0.2.0"}, {"status", "loaded"}
    };

    auto cmd = createCommand("info", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastInfoModule, "chat");
}

TEST_F(CommandTest, ModuleInfo_NotFound)
{
    mockClient.moduleInfoResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_NOT_FOUND"},
        {"message", "Module 'nonexistent' not found."}
    };

    auto cmd = createCommand("module-info", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"nonexistent"});
        EXPECT_EQ(exitCode, 3);
    });
}

// ── call ─────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Call_Success)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "ok"}, {"module", "chat"}, {"method", "send_message"},
        {"result", "message sent (id: msg_123)"}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "send_message", "hello"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastCallModule, "chat");
    EXPECT_EQ(mockClient.lastCallMethod, "send_message");
    EXPECT_EQ(mockClient.lastCallArgs.size(), 1u);
}

TEST_F(CommandTest, Call_VerboseSyntax)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "ok"}, {"module", "chat"}, {"method", "send_message"}
    };

    auto cmd = createCommand("module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "method", "send_message", "hello"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastCallModule, "chat");
    EXPECT_EQ(mockClient.lastCallMethod, "send_message");
}

TEST_F(CommandTest, Call_MethodNotFound)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "error"}, {"code", "METHOD_NOT_FOUND"},
        {"message", "Method 'bad' not found on module 'chat'."}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "bad"});
        EXPECT_EQ(exitCode, 4);
    });
}

TEST_F(CommandTest, Call_ModuleNotLoaded)
{
    mockClient.callMethodResult = LogosMap{
        {"status", "error"}, {"code", "MODULE_NOT_LOADED"},
        {"message", "Module 'delivery' is not loaded."}
    };

    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"delivery", "send_package"});
        EXPECT_EQ(exitCode, 3);
    });
}

TEST_F(CommandTest, Call_MissingArgs)
{
    auto cmd = createCommand("call", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });
}

// ── call: argument type coercion ─────────────────────────────────────────────
// Args reach the daemon as native JSON types. Regression guard: a decimal
// like "1.25" must NOT be truncated to int 1 (std::stoi accepts a numeric
// prefix), so the whole string has to be consumed for a numeric type.

TEST_F(CommandTest, Call_CoercesDecimalArgsToDouble)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}, {"result", 4.0}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"calc", "addDoubles", "1.25", "2.75"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[0].get<double>(), 1.25);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[1].get<double>(), 2.75);
}

TEST_F(CommandTest, Call_CoercesIntegerArgsToInt)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"calc", "addInts", "3", "-4"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int>(), 3);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[1].get<int>(), -4);
}

// LIDL `int`/`uint` are 64-bit, so an argument has to survive the full range.
// This used std::stoi (32 bits): every integer outside int32 threw out_of_range,
// was swallowed, and came back out of std::stod as a DOUBLE. Under 2^53 that is
// exact and looks correct, which is why it went unnoticed — above it the value
// is silently rounded. Found by probing a live provider:
//   echoUint 9007199254740993 -> 9007199254740992
TEST_F(CommandTest, Call_KeepsSixtyFourBitIntegerArgsExact)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f",
                                "4294967296",            // > int32max
                                "9007199254740993",      // > 2^53: double rounds it
                                "9223372036854775807",   // int64max
                                "-9223372036854775808"}), // int64min
                  0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 4u);
    for (const auto& a : mockClient.lastCallArgs)
        EXPECT_TRUE(a.is_number_integer()) << "arg reached the daemon as " << a.dump();
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int64_t>(), 4294967296LL);
    EXPECT_EQ(mockClient.lastCallArgs[1].get<int64_t>(), 9007199254740993LL);
    EXPECT_EQ(mockClient.lastCallArgs[2].get<int64_t>(), 9223372036854775807LL);
    EXPECT_EQ(mockClient.lastCallArgs[3].get<int64_t>(), INT64_MIN);
}

// The band above int64max is still a valid `uint`. stoull is tried only for a
// non-negative literal — stoull("-1") wraps to uint64max rather than failing.
TEST_F(CommandTest, Call_CoercesAboveInt64MaxToUnsigned)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "18446744073709551615", "-1"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_unsigned());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<uint64_t>(), 18446744073709551615ULL);
    // Still signed, NOT wrapped.
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[1].get<int64_t>(), -1);
}

// Beyond uint64 there is no integer type left; a numeric literal that fits
// neither still goes as a double rather than a string.
TEST_F(CommandTest, Call_FallsBackToDoubleBeyondUint64)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "99999999999999999999999"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_float());
}

TEST_F(CommandTest, Call_CoercesMixedArgTypes)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "hi", "3.0", "true", "5"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 4u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "hi");
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[1].get<double>(), 3.0);
    EXPECT_TRUE(mockClient.lastCallArgs[2].is_boolean());
    EXPECT_TRUE(mockClient.lastCallArgs[2].get<bool>());
    EXPECT_TRUE(mockClient.lastCallArgs[3].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[3].get<int>(), 5);
}

TEST_F(CommandTest, Call_TrimsWhitespaceForNumericCoercion)
{
    // @file params commonly arrive with a trailing newline; "123\n" must still
    // coerce to a number rather than fall through to a string.
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "123\n", " 1.5 "}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 2u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int>(), 123);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_number_float());
    EXPECT_DOUBLE_EQ(mockClient.lastCallArgs[1].get<double>(), 1.5);
}

// ── call: json: / str: argument prefixes ─────────────────────────────────────
// Scalar coercion can only produce bool/int/double/string, so `json:<value>`
// opts into JSON parsing (list / map / nested), `json:@file` parses file
// contents, and `str:<text>` forces a literal string past all coercion. These
// are the two explicit escapes that make containers and every literal string
// expressible from the command line.

TEST_F(CommandTest, Call_JsonPrefixParsesList)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "echoList", "json:[1,2,3]"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_array());
    EXPECT_EQ(mockClient.lastCallArgs[0], (LogosList{1, 2, 3}));
    // Integers inside the list must stay integers, not degrade to double.
    EXPECT_TRUE(mockClient.lastCallArgs[0][0].is_number_integer());
}

TEST_F(CommandTest, Call_JsonPrefixParsesMap)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "echoMap", R"(json:{"k":"v","n":42})"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_object());
    EXPECT_EQ(mockClient.lastCallArgs[0].value("k", std::string{}), "v");
    EXPECT_EQ(mockClient.lastCallArgs[0].value("n", 0), 42);
}

TEST_F(CommandTest, Call_JsonPrefixParsesNestedAndScalars)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        // json: also expresses a bare scalar unambiguously (a real int/bool),
        // and a nested value the default path could never produce.
        EXPECT_EQ(cmd->execute({"m", "f", "json:42", "json:true",
                                R"(json:{"a":[1,{"b":2}]})"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 3u);
    EXPECT_TRUE(mockClient.lastCallArgs[0].is_number_integer());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<int>(), 42);
    EXPECT_TRUE(mockClient.lastCallArgs[1].is_boolean());
    EXPECT_TRUE(mockClient.lastCallArgs[1].get<bool>());
    EXPECT_EQ(mockClient.lastCallArgs[2]["a"][1]["b"].get<int>(), 2);
}

TEST_F(CommandTest, Call_JsonPrefixFromFile)
{
    const std::string path = testing::TempDir() + "logosctl_call_json_arg.json";
    { std::ofstream f(path); f << "[10, 20, 30]"; }
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "echoList", "json:@" + path}), 0);
    });
    std::remove(path.c_str());
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_array());
    EXPECT_EQ(mockClient.lastCallArgs[0], (LogosList{10, 20, 30}));
}

TEST_F(CommandTest, Call_JsonPrefixMalformedErrors)
{
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 0;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "json:hello"});
    });
    EXPECT_EQ(exitCode, 1);
    // Rejected before the RPC — the method was never dialed.
    EXPECT_NE(mockClient.lastCallMethod, "f");
}

TEST_F(CommandTest, Call_JsonPrefixMissingFileErrors)
{
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 0;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "json:@/no/such/logosctl/file.json"});
    });
    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(mockClient.lastCallMethod, "f");
}

TEST_F(CommandTest, Call_StrPrefixForcesLiteralString)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        // Every value the default path would otherwise reinterpret — a
        // json:-looking string, a number, a bool, an @file reference — stays a
        // literal string under str:, with the prefix stripped.
        EXPECT_EQ(cmd->execute({"m", "f", "str:json:x", "str:42",
                                "str:true", "str:@config.json"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 4u);
    for (const auto& a : mockClient.lastCallArgs)
        EXPECT_TRUE(a.is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "json:x");
    EXPECT_EQ(mockClient.lastCallArgs[1].get<std::string>(), "42");
    EXPECT_EQ(mockClient.lastCallArgs[2].get<std::string>(), "true");
    EXPECT_EQ(mockClient.lastCallArgs[3].get<std::string>(), "@config.json");
}

TEST_F(CommandTest, Call_StrPrefixExpressesEmptyString)
{
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    captureOutput([&]() {
        EXPECT_EQ(cmd->execute({"m", "f", "str:"}), 0);
    });
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "");
}

TEST_F(CommandTest, Call_EmptyFileYieldsEmptyStringNotError)
{
    // A readable-but-empty @file is a successful read of "", distinct from an
    // unreadable file (which errors). Regression guard for resolveFileParam's
    // couldn't-open vs read-but-empty distinction.
    const std::string path = testing::TempDir() + "logosctl_call_empty_arg";
    { std::ofstream f(path); }  // create empty
    mockClient.callMethodResult = LogosMap{{"status", "ok"}};
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 1;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "@" + path});
    });
    std::remove(path.c_str());
    EXPECT_EQ(exitCode, 0);
    ASSERT_EQ(mockClient.lastCallArgs.size(), 1u);
    ASSERT_TRUE(mockClient.lastCallArgs[0].is_string());
    EXPECT_EQ(mockClient.lastCallArgs[0].get<std::string>(), "");
}

TEST_F(CommandTest, Call_MissingFileErrors)
{
    auto cmd = createCommand("call", mockClient, output);
    int exitCode = 0;
    captureOutput([&]() {
        exitCode = cmd->execute({"m", "f", "@/no/such/logosctl/file.txt"});
    });
    EXPECT_EQ(exitCode, 1);
    EXPECT_NE(mockClient.lastCallMethod, "f");
}

// ── stats ────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Stats_Success)
{
    mockClient.moduleStatsResult = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"pid", 23456}, {"cpu_percent", 2.1}, {"memory_mb", 48.3}},
        LogosMap{{"name", "chat"}, {"pid", 23457}, {"cpu_percent", 0.4}, {"memory_mb", 22.1}}
    });

    auto cmd = createCommand("stats", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    nlohmann::json doc = parseJson(out);
    ASSERT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 2u);
}

// ── watch ────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Watch_MissingArgs)
{
    auto cmd = createCommand("watch", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });
}

TEST_F(CommandTest, Watch_ParsesModuleAndEventName)
{
    auto cmd = createCommand("watch", mockClient, output);
    captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "--event", "message"});
        EXPECT_EQ(exitCode, 3);  // watchShouldSucceed=false => WATCH_FAILED
    });

    EXPECT_EQ(mockClient.lastWatchModule,    "chat");
    EXPECT_EQ(mockClient.lastWatchEventName, "message");
}

TEST_F(CommandTest, Watch_ParsesModuleOnly)
{
    auto cmd = createCommand("watch", mockClient, output);
    captureOutput([&]() {
        cmd->execute({"waku"});
    });

    EXPECT_EQ(mockClient.lastWatchModule,    "waku");
    EXPECT_EQ(mockClient.lastWatchEventName, "");
}

TEST_F(CommandTest, Watch_ModuleNotLoaded_ReturnsExit3)
{
    auto cmd = createCommand("watch", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"missing"});
        EXPECT_EQ(exitCode, 3);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "WATCH_FAILED");
    EXPECT_NE(doc["message"].get<std::string>().find("'missing'"), std::string::npos);
}

TEST_F(CommandTest, Watch_NoDaemon_ReturnsExit2)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("watch", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"chat", "--event", "message"});
        EXPECT_EQ(exitCode, 2);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

// ── stop ─────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Stop_Success)
{
    mockClient.shutdownResult = LogosMap{
        {"status", "ok"}, {"message", "Daemon shutting down."}
    };

    auto cmd = createCommand("stop", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_TRUE(mockClient.shutdownCalled);

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
}

TEST_F(CommandTest, Stop_NoDaemon)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("stop", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 2);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "NO_DAEMON");
}

// ---------------------------------------------------------------------------
// package download
//
// These exist because `-o` was parsed and then thrown away — the option was
// accepted, the file went to $TMPDIR, and nothing anywhere said so. There was
// no unit coverage of PackageCommand at all, which is why it survived.
// ---------------------------------------------------------------------------

TEST_F(CommandTest, PackageDownload_PassesOutputDirectoryThrough)
{
    mockClient.downloadResult = LogosMap{
        {"status", "ok"},
        {"result", LogosMap{{"name", "storage_module"}, {"path", "/out/storage_module.lgx"}}}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() {
        int exitCode = cmd->execute({"download", "storage_module", "-o", "/out"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastDownloadName, "storage_module");
    EXPECT_EQ(mockClient.lastDownloadOpts.value("output", std::string{}), "/out");
}

// A relative -o has to become absolute before it leaves this process: the
// daemon does the move, and its working directory is not ours -- for a
// detached daemon it is wherever it happened to be started.
TEST_F(CommandTest, PackageDownload_ResolvesRelativeOutputAgainstOurCwd)
{
    mockClient.downloadResult = LogosMap{
        {"status", "ok"}, {"result", LogosMap{{"path", "/x/p.lgx"}}}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() { cmd->execute({"download", "pkg", "-o", "pkgs"}); });

    const std::string sent = mockClient.lastDownloadOpts.value("output", std::string{});
    ASSERT_FALSE(sent.empty());
    EXPECT_EQ(sent, (std::filesystem::current_path() / "pkgs").string())
        << "a relative -o must be resolved against the client's cwd, not sent raw";
}

// No -o means "the session's cache", which only the daemon knows the path of.
// Sending an empty string is how it is told to use that default.
TEST_F(CommandTest, PackageDownload_NoOutputDirLeavesTheChoiceToTheDaemon)
{
    mockClient.downloadResult = LogosMap{
        {"status", "ok"}, {"result", LogosMap{{"path", "/cache/downloads/p.lgx"}}}};

    auto cmd = createCommand("package", mockClient, output);
    captureOutput([&]() { cmd->execute({"download", "pkg"}); });

    EXPECT_EQ(mockClient.lastDownloadOpts.value("output", std::string("unset")), "");
}

TEST_F(CommandTest, PackageDownload_ReportsFailure)
{
    mockClient.downloadResult = LogosMap{
        {"status", "error"}, {"code", "DOWNLOAD_FAILED"}, {"message", "no such package"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"download", "nope"});
        EXPECT_EQ(exitCode, 1);
    });

    nlohmann::json doc = parseJson(out);
    EXPECT_EQ(doc["code"].get<std::string>(), "DOWNLOAD_FAILED");
}

// ── package failure reporting ───────────────────────────────────────────────
//
// A real install failure was reported as "install failed at step '?': " with
// nothing after the colon. The daemon had said why; the client dropped it,
// because package_ops returns two different error shapes and this only read
// one. The reason is the entire value of the message.

TEST_F(CommandTest, PackageMutate_ReportsTheReasonFromEitherErrorShape)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","blockchain_module"},
                                               {"action","install"},
                                               {"toVersion","0.2.1"}}})},
        {"affected_loaded", LogosList::array()}};

    // The shape produced before the step chain starts: code + message.
    mockClient.applyPackageResult = LogosMap{
        {"status", "error"},
        {"code", "DOWNLOAD_FAILED"},
        {"message", "package_downloader did not respond"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"install", "blockchain_module", "-y"});
        EXPECT_EQ(exitCode, 1);
    });

    nlohmann::json doc = parseJson(out);
    const std::string msg = doc["message"].get<std::string>();
    EXPECT_NE(msg.find("package_downloader did not respond"), std::string::npos)
        << "the daemon's reason must survive to the user; got: " << msg;
    EXPECT_EQ(msg.find("step '?'"), std::string::npos)
        << "an unknown step should be omitted, not printed as '?': " << msg;
}

TEST_F(CommandTest, PackageMutate_KeepsTheStepWhenTheDaemonReportsOne)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","storage_module"},
                                               {"action","install"}}})},
        {"affected_loaded", LogosList::array()}};

    // The shape package_ops' own `fail()` produces: failed_step + error.
    mockClient.applyPackageResult = LogosMap{
        {"status", "error"},
        {"failed_step", "confirm"},
        {"error", "package_manager rejected the install"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() { cmd->execute({"install", "storage_module", "-y"}); });

    const std::string msg = parseJson(out)["message"].get<std::string>();
    EXPECT_NE(msg.find("confirm"), std::string::npos) << msg;
    EXPECT_NE(msg.find("package_manager rejected the install"), std::string::npos) << msg;
}

// Never print a bare colon with nothing after it: if both shapes are empty we
// still owe the reader a sentence.
TEST_F(CommandTest, PackageMutate_SaysSoWhenNoReasonWasReported)
{
    mockClient.planPackageResult = LogosMap{
        {"status", "ok"},
        {"changes", LogosList::array({LogosMap{{"name","x"},{"action","install"}}})},
        {"affected_loaded", LogosList::array()}};
    mockClient.applyPackageResult = LogosMap{{"status", "error"}};

    auto cmd = createCommand("package", mockClient, output);
    std::string out = captureOutput([&]() { cmd->execute({"install", "x", "-y"}); });

    const std::string msg = parseJson(out)["message"].get<std::string>();
    EXPECT_NE(msg.find("no reason reported"), std::string::npos) << msg;
}
