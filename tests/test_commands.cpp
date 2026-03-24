#include <gtest/gtest.h>
#include <QJsonObject>
#include <QJsonArray>
#include <sstream>
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"

// Mock client for testing commands without a real daemon
class MockClient : public Client {
public:
    // Control mock behavior
    bool shouldConnect = true;
    QString connectError = "No running logoscore daemon. Start one with: logoscore -D";
    QJsonObject loadModuleResult;
    QJsonObject unloadModuleResult;
    QJsonObject reloadModuleResult;
    QJsonArray listModulesResult;
    QJsonObject statusResult;
    QJsonObject moduleInfoResult;
    QJsonArray moduleStatsResult;
    QJsonObject callMethodResult;
    QJsonObject shutdownResult;

    // Track calls
    bool shutdownCalled = false;
    QString lastLoadedModule;
    QString lastUnloadedModule;
    QString lastReloadedModule;
    QString lastInfoModule;
    QString lastCallModule;
    QString lastCallMethod;
    QVariantList lastCallArgs;
    QString lastListFilter;

    bool connect() override {
        m_connected = shouldConnect;
        m_lastError = shouldConnect ? "" : connectError;
        return shouldConnect;
    }

    bool isConnected() const override { return m_connected; }
    QString lastError() const override { return m_lastError; }

    QJsonObject loadModule(const QString& name) override {
        lastLoadedModule = name;
        return loadModuleResult;
    }

    QJsonObject unloadModule(const QString& name) override {
        lastUnloadedModule = name;
        return unloadModuleResult;
    }

    QJsonObject reloadModule(const QString& name) override {
        lastReloadedModule = name;
        return reloadModuleResult;
    }

    QJsonArray listModules(const QString& filter) override {
        lastListFilter = filter;
        return listModulesResult;
    }

    QJsonObject getStatus() override { return statusResult; }

    QJsonObject getModuleInfo(const QString& name) override {
        lastInfoModule = name;
        return moduleInfoResult;
    }

    QJsonArray getModuleStats() override { return moduleStatsResult; }

    QJsonObject callModuleMethod(const QString& module, const QString& method,
                                  const QVariantList& args) override {
        lastCallModule = module;
        lastCallMethod = method;
        lastCallArgs = args;
        return callMethodResult;
    }

    QJsonObject shutdown() override {
        shutdownCalled = true;
        return shutdownResult;
    }

    bool watchModuleEvents(const QString& module, const QString& eventName,
                            std::function<void(const QJsonObject&)> callback) override {
        Q_UNUSED(module); Q_UNUSED(eventName); Q_UNUSED(callback);
        return m_connected;
    }

private:
    bool m_connected = false;
    QString m_lastError;
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
};

// ── createCommand ────────────────────────────────────────────────────────────

TEST_F(CommandTest, CreateCommand_KnownCommands)
{
    EXPECT_NE(createCommand("status", mockClient, output), nullptr);
    EXPECT_NE(createCommand("load-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("unload-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("reload-module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("list-modules", mockClient, output), nullptr);
    EXPECT_NE(createCommand("module-info", mockClient, output), nullptr);
    EXPECT_NE(createCommand("info", mockClient, output), nullptr);
    EXPECT_NE(createCommand("call", mockClient, output), nullptr);
    EXPECT_NE(createCommand("module", mockClient, output), nullptr);
    EXPECT_NE(createCommand("watch", mockClient, output), nullptr);
    EXPECT_NE(createCommand("stats", mockClient, output), nullptr);
    EXPECT_NE(createCommand("stop", mockClient, output), nullptr);
}

TEST_F(CommandTest, CreateCommand_Unknown_ReturnsNull)
{
    EXPECT_EQ(createCommand("nonexistent", mockClient, output), nullptr);
}

// ── knownSubcommands ─────────────────────────────────────────────────────────

TEST_F(CommandTest, KnownSubcommands_ContainsExpected)
{
    QStringList cmds = knownSubcommands();
    EXPECT_TRUE(cmds.contains("status"));
    EXPECT_TRUE(cmds.contains("load-module"));
    EXPECT_TRUE(cmds.contains("unload-module"));
    EXPECT_TRUE(cmds.contains("reload-module"));
    EXPECT_TRUE(cmds.contains("list-modules"));
    EXPECT_TRUE(cmds.contains("module-info"));
    EXPECT_TRUE(cmds.contains("info"));
    EXPECT_TRUE(cmds.contains("call"));
    EXPECT_TRUE(cmds.contains("watch"));
    EXPECT_TRUE(cmds.contains("stats"));
    EXPECT_TRUE(cmds.contains("stop"));
    EXPECT_TRUE(cmds.contains("daemon"));
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

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    EXPECT_EQ(doc.object().value("code").toString(), "NO_DAEMON");
}

// ── load-module ──────────────────────────────────────────────────────────────

TEST_F(CommandTest, LoadModule_Success)
{
    mockClient.loadModuleResult = QJsonObject{
        {"status", "ok"}, {"module", "waku"}, {"version", "0.1.0"},
        {"dependencies_loaded", QJsonArray{"store"}}
    };

    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastLoadedModule, "waku");

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    EXPECT_EQ(doc.object().value("status").toString(), "ok");
    EXPECT_EQ(doc.object().value("module").toString(), "waku");
}

TEST_F(CommandTest, LoadModule_NotFound)
{
    mockClient.loadModuleResult = QJsonObject{
        {"status", "error"}, {"code", "MODULE_NOT_FOUND"},
        {"message", "Module 'nonexistent' not found."}
    };

    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"nonexistent"});
        EXPECT_EQ(exitCode, 3);
    });

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    EXPECT_EQ(doc.object().value("code").toString(), "MODULE_NOT_FOUND");
}

TEST_F(CommandTest, LoadModule_MissingArg)
{
    auto cmd = createCommand("load-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 1);
    });

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    EXPECT_EQ(doc.object().value("code").toString(), "INVALID_ARGS");
}

// ── unload-module ────────────────────────────────────────────────────────────

TEST_F(CommandTest, UnloadModule_Success)
{
    mockClient.unloadModuleResult = QJsonObject{
        {"status", "ok"}, {"module", "waku"}
    };

    auto cmd = createCommand("unload-module", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({"waku"});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastUnloadedModule, "waku");
}

// ── reload-module ────────────────────────────────────────────────────────────

TEST_F(CommandTest, ReloadModule_Success)
{
    mockClient.reloadModuleResult = QJsonObject{
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
    mockClient.reloadModuleResult = QJsonObject{
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
    mockClient.listModulesResult = QJsonArray{
        QJsonObject{{"name", "waku"}, {"status", "loaded"}},
        QJsonObject{{"name", "chat"}, {"status", "not_loaded"}}
    };

    auto cmd = createCommand("list-modules", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_EQ(mockClient.lastListFilter, "all");
}

TEST_F(CommandTest, ListModules_LoadedFilter)
{
    mockClient.listModulesResult = QJsonArray{
        QJsonObject{{"name", "waku"}, {"status", "loaded"}}
    };

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
    mockClient.moduleInfoResult = QJsonObject{
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
    mockClient.moduleInfoResult = QJsonObject{
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
    mockClient.moduleInfoResult = QJsonObject{
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
    mockClient.callMethodResult = QJsonObject{
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
    EXPECT_EQ(mockClient.lastCallArgs.size(), 1);
}

TEST_F(CommandTest, Call_VerboseSyntax)
{
    mockClient.callMethodResult = QJsonObject{
        {"status", "ok"}, {"module", "chat"}, {"method", "send_message"}
    };

    // "module <name> method <method> [args...]" -> args = [<name>, "method", <method>, args...]
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
    mockClient.callMethodResult = QJsonObject{
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
    mockClient.callMethodResult = QJsonObject{
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

// ── stats ────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Stats_Success)
{
    mockClient.moduleStatsResult = QJsonArray{
        QJsonObject{{"name", "waku"}, {"pid", 23456}, {"cpu_percent", 2.1}, {"memory_mb", 48.3}},
        QJsonObject{{"name", "chat"}, {"pid", 23457}, {"cpu_percent", 0.4}, {"memory_mb", 22.1}}
    };

    auto cmd = createCommand("stats", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isArray());
    EXPECT_EQ(doc.array().size(), 2);
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

// ── stop ─────────────────────────────────────────────────────────────────────

TEST_F(CommandTest, Stop_Success)
{
    mockClient.shutdownResult = QJsonObject{
        {"status", "ok"}, {"message", "Daemon shutting down."}
    };

    auto cmd = createCommand("stop", mockClient, output);
    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 0);
    });

    EXPECT_TRUE(mockClient.shutdownCalled);

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    EXPECT_EQ(doc.object().value("status").toString(), "ok");
}

TEST_F(CommandTest, Stop_NoDaemon)
{
    mockClient.shouldConnect = false;
    auto cmd = createCommand("stop", mockClient, output);

    std::string out = captureOutput([&]() {
        int exitCode = cmd->execute({});
        EXPECT_EQ(exitCode, 2);
    });

    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    EXPECT_EQ(doc.object().value("code").toString(), "NO_DAEMON");
}
