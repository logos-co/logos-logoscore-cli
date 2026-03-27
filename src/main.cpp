#include <CLI/CLI.hpp>
#include <QCoreApplication>
#include <QDebug>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "config.h"
#include "daemon/daemon.h"
#include "daemon/connection_file.h"
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"
#include "inline/command_line_parser.h"
#include "inline/call_executor.h"
#include "logos_core.h"

static bool g_verbose = false;

static void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    QByteArray localMsg = msg.toLocal8Bit();
    const char *file = context.file ? context.file : "";
    const char *function = context.function ? context.function : "";

    switch (type) {
    case QtDebugMsg:
        if (!g_verbose) return;
        fprintf(stderr, "Debug: %s\n", localMsg.constData());
        break;
    case QtInfoMsg:
        if (!g_verbose) return;
        fprintf(stderr, "Info: %s\n", localMsg.constData());
        break;
    case QtWarningMsg:
        if (!g_verbose) return;
        fprintf(stderr, "Warning: %s\n", localMsg.constData());
        break;
    case QtCriticalMsg:
        fprintf(stderr, "Critical: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
        break;
    case QtFatalMsg:
        fprintf(stderr, "Fatal: %s (%s:%u, %s)\n", localMsg.constData(), file, context.line, function);
        fflush(stderr);
        abort();
    }
    fflush(stderr);
}

static int runInlineMode(int argc, char* argv[],
                         const std::vector<std::string>& modulesDirs,
                         const std::string& loadModulesStr,
                         const std::vector<std::string>& callStrs,
                         bool quitOnFinish)
{
    // Build CoreArgs from pre-parsed CLI11 values
    CoreArgs args;
    args.valid = true;
    args.quitOnFinish = quitOnFinish;
    args.modulesDirs = modulesDirs;

    // Parse comma-separated module names
    if (!loadModulesStr.empty()) {
        std::string current;
        for (char c : loadModulesStr) {
            if (c == ',') {
                if (!current.empty()) {
                    args.loadModules.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            args.loadModules.push_back(current);
        }
    }

    // Parse call strings
    for (const auto& callStr : callStrs) {
        ModuleCall call = parseCallString(callStr);
        if (!call.moduleName.empty() && !call.methodName.empty()) {
            args.calls.push_back(call);
        } else {
            fprintf(stderr, "Skipping invalid call: %s\n", callStr.c_str());
        }
    }

    QCoreApplication app(argc, argv);
    app.setApplicationName("logoscore");
    app.setApplicationVersion("1.0");

    logos_core_init(argc, argv);

    for (const std::string& dir : args.modulesDirs) {
        std::error_code ec;
        std::string absDir = std::filesystem::absolute(dir, ec).string();
        logos_core_add_plugins_dir(ec ? dir.c_str() : absDir.c_str());
    }

    QByteArray bundledDir = qgetenv("LOGOS_BUNDLED_MODULES_DIR");
    if (!bundledDir.isEmpty()) {
        logos_core_add_plugins_dir(bundledDir.constData());
    }

    logos_core_start();

    if (!args.loadModules.empty()) {
        for (const std::string& moduleName : args.loadModules) {
            if (moduleName.empty())
                continue;
            if (!logos_core_load_plugin_with_dependencies(moduleName.c_str())) {
                qWarning() << "Failed to load module:" << QString::fromStdString(moduleName);
            }
        }
    }

    if (!args.calls.empty()) {
        int callResult = CallExecutor::executeCalls(args.calls);
        if (args.quitOnFinish || callResult != 0) {
            return callResult;
        }
    }

    return logos_core_exec();
}

int main(int argc, char *argv[])
{
    // ── CLI11 setup ──────────────────────────────────────────────────────────
    CLI::App app{"logoscore - Logos Core runtime CLI"};
    app.set_version_flag("--version", "logoscore version 1.0");
    app.set_help_flag("-h,--help", "Show this help");

    // Global flags
    app.add_flag("-v,--verbose", g_verbose, "Show debug logs");

    // Client global flags (defined at app level so they work before subcommands;
    // also extracted from subcommand remaining() for placement after subcommands)
    bool jsonMode = false;
    app.add_flag("-j,--json", jsonMode, "Force JSON output");

    bool quiet = false;
    app.add_flag("-q,--quiet", quiet, "Suppress non-essential output");

    // Daemon flag (-D as shorthand for daemon subcommand)
    bool daemonFlag = false;
    app.add_flag("-D", daemonFlag, "Start the daemon process");

    // Shared option: modules directory (used by daemon + inline)
    std::vector<std::string> modulesDirs;
    app.add_option("-m,--modules-dir", modulesDirs, "Module search directory (repeatable)");

    // Inline mode options
    std::string loadModulesStr;
    app.add_option("-l,--load-modules", loadModulesStr, "Comma-separated modules to load");

    std::vector<std::string> callStrs;
    app.add_option("-c,--call", callStrs, "Call module.method(args) (repeatable)");

    bool quitOnFinish = false;
    app.add_flag("--quit-on-finish", quitOnFinish, "Exit after calls complete");

    // ── Client subcommands ───────────────────────────────────────────────────
    // All client subcommands use allow_extras() so their positional args and
    // command-specific flags (--loaded, --event) are captured in remaining().
    // Global flags (--json, --quiet) mixed in after the subcommand are also
    // captured and extracted before dispatching to the command object.

    auto* daemonSub  = app.add_subcommand("daemon", "Start the daemon process");
    daemonSub->fallthrough();  // -m, -v after "daemon" fall through to parent

    auto* statusSub        = app.add_subcommand("status", "Show daemon and module health");
    auto* loadModuleSub    = app.add_subcommand("load-module", "Load a module into the daemon");
    auto* unloadModuleSub  = app.add_subcommand("unload-module", "Unload a module from the daemon");
    auto* reloadModuleSub  = app.add_subcommand("reload-module", "Reload (unload + load) a module");
    auto* listModulesSub   = app.add_subcommand("list-modules", "List available or loaded modules");
    auto* moduleInfoSub    = app.add_subcommand("module-info", "Show detailed module information");
    auto* infoSub          = app.add_subcommand("info", "Alias for module-info");
    auto* callSub          = app.add_subcommand("call", "Call a method on a loaded module");
    auto* moduleSub        = app.add_subcommand("module", "Call a method (verbose syntax)");
    auto* watchSub         = app.add_subcommand("watch", "Watch events from a module");
    auto* statsSub         = app.add_subcommand("stats", "Show module resource usage");
    auto* stopSub          = app.add_subcommand("stop", "Stop the daemon");

    // Allow extras on all client subcommands so their positional args and
    // command-specific flags pass through to the Command objects unchanged
    for (auto* sub : {statusSub, loadModuleSub, unloadModuleSub, reloadModuleSub,
                      listModulesSub, moduleInfoSub, infoSub, callSub, moduleSub,
                      watchSub, statsSub, stopSub}) {
        sub->allow_extras();
    }

    app.require_subcommand(0, 1);  // 0 or 1 subcommand

    // ── Parse ────────────────────────────────────────────────────────────────
    CLI11_PARSE(app, argc, argv);

    qInstallMessageHandler(messageHandler);

    // ── Daemon mode ──────────────────────────────────────────────────────────
    if (daemonFlag || daemonSub->parsed()) {
        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion("1.0");
        return Daemon::start(argc, argv, modulesDirs);
    }

    // ── Client mode ──────────────────────────────────────────────────────────
    struct SubInfo { CLI::App* sub; QString name; };
    std::vector<SubInfo> clientSubs = {
        {statusSub, "status"},
        {loadModuleSub, "load-module"},
        {unloadModuleSub, "unload-module"},
        {reloadModuleSub, "reload-module"},
        {listModulesSub, "list-modules"},
        {moduleInfoSub, "module-info"},
        {infoSub, "info"},
        {callSub, "call"},
        {moduleSub, "module"},
        {watchSub, "watch"},
        {statsSub, "stats"},
        {stopSub, "stop"},
    };

    for (auto& [sub, name] : clientSubs) {
        if (!sub->parsed())
            continue;

        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion("1.0");

        // Collect remaining args from the subcommand, extracting global flags
        // (global flags placed after the subcommand end up in remaining())
        std::vector<std::string> cmdArgs;

        for (const auto& r : sub->remaining()) {
            if (r == "--json" || r == "-j") {
                jsonMode = true;
            } else if (r == "--quiet" || r == "-q") {
                quiet = true;
            } else {
                cmdArgs.push_back(r);
            }
        }

        Output output(jsonMode);
        RpcClient rpcClient;

        auto cmd = createCommand(name, rpcClient, output);
        if (!cmd) {
            output.printError("INVALID_ARGS",
                             QString("Unknown command: %1. Run 'logoscore --help' for usage.").arg(name));
            return 1;
        }

        return cmd->execute(cmdArgs);
    }

    // ── Inline mode (legacy) ─────────────────────────────────────────────────
    if (!modulesDirs.empty() || !loadModulesStr.empty() || !callStrs.empty() || quitOnFinish) {
        return runInlineMode(argc, argv, modulesDirs, loadModulesStr, callStrs, quitOnFinish);
    }

    // ── No mode detected — show help ─────────────────────────────────────────
    std::cout << app.help() << std::endl;
    return 0;
}
