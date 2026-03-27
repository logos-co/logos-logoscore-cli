#include <QCoreApplication>
#include <QDebug>
#include <cstdio>
#include <cstring>
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

enum class Mode {
    Daemon,
    Client,
    Inline,
    Help,
    Version
};

static Mode detectMode(int argc, char* argv[])
{
    // Scan argv for mode indicators
    bool hasDaemonFlag = false;
    bool hasInlineFlags = false;
    std::string firstPositionalArg;

    QStringList subcommands = knownSubcommands();

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "-D" || arg == "daemon") {
            hasDaemonFlag = true;
            break;
        }
        if (arg == "--help" || arg == "-h") {
            return Mode::Help;
        }
        if (arg == "--version") {
            return Mode::Version;
        }
        if (arg == "-m" || arg == "--modules-dir" ||
            arg == "-l" || arg == "--load-modules" ||
            arg == "-c" || arg == "--call" ||
            arg == "--quit-on-finish") {
            hasInlineFlags = true;
        }
        // Check if first positional arg is a known subcommand
        if (arg[0] != '-' && firstPositionalArg.empty()) {
            firstPositionalArg = arg;
        }
    }

    if (hasDaemonFlag)
        return Mode::Daemon;

    if (!firstPositionalArg.empty() && subcommands.contains(QString::fromStdString(firstPositionalArg)))
        return Mode::Client;

    if (hasInlineFlags)
        return Mode::Inline;

    return Mode::Help;
}

static void printHelp()
{
    std::cout << "logoscore - Logos Core runtime CLI\n"
              << "\n"
              << "Usage:\n"
              << "  logoscore -D [--modules-dir <path>]...       Start daemon\n"
              << "  logoscore <command> [flags] [args...]        Run a command\n"
              << "  logoscore -m <path> -l <mods> -c <call>     Inline mode (legacy)\n"
              << "\n"
              << "Commands:\n"
              << "  status                  Show daemon and module health\n"
              << "  load-module <name>      Load a module into the daemon\n"
              << "  unload-module <name>    Unload a module from the daemon\n"
              << "  reload-module <name>    Reload (unload + load) a module\n"
              << "  list-modules [--loaded] List available or loaded modules\n"
              << "  module-info <name>      Show detailed module information\n"
              << "  info <name>             Alias for module-info\n"
              << "  call <mod> <method>     Call a method on a loaded module\n"
              << "  watch <mod> [--event]   Watch events from a module\n"
              << "  stats                   Show module resource usage\n"
              << "  stop                    Stop the daemon\n"
              << "\n"
              << "Global Flags:\n"
              << "  -j, --json              Force JSON output\n"
              << "  -q, --quiet             Suppress non-essential output\n"
              << "  -v, --verbose           Show debug logs\n"
              << "  -h, --help              Show this help\n"
              << "      --version           Show version\n"
              << "\n"
              << "Daemon Flags:\n"
              << "  -D, daemon              Start the daemon process\n"
              << "  -m, --modules-dir       Module search directory (repeatable)\n"
              << "\n"
              << "Legacy (Inline) Flags:\n"
              << "  -m, --modules-dir       Module search directory (repeatable)\n"
              << "  -l, --load-modules      Comma-separated modules to load\n"
              << "  -c, --call              Call module.method(args)\n"
              << "      --quit-on-finish    Exit after calls complete\n"
              << std::endl;
}

static int runClientMode(int argc, char* argv[])
{
    // Parse global flags and subcommand
    bool jsonMode = false;
    bool quiet = false;
    std::string subcommand;
    QStringList cmdArgs;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--json" || arg == "-j") {
            jsonMode = true;
            continue;
        }
        if (arg == "--quiet" || arg == "-q") {
            quiet = true;
            continue;
        }
        if (arg == "--verbose" || arg == "-v") {
            continue; // already handled in main()
        }

        if (subcommand.empty() && arg[0] != '-') {
            subcommand = arg;
        } else if (!subcommand.empty()) {
            cmdArgs.append(QString::fromUtf8(argv[i]));
        }
    }

    Output output(jsonMode);
    RpcClient rpcClient;

    auto cmd = createCommand(QString::fromStdString(subcommand), rpcClient, output);
    if (!cmd) {
        output.printError("INVALID_ARGS",
                         QString("Unknown command: %1. Run 'logoscore --help' for usage.").arg(QString::fromStdString(subcommand)));
        return 1;
    }

    return cmd->execute(cmdArgs);
}

static int runDaemonMode(int argc, char* argv[])
{
    QStringList modulesDirs;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "-m" || arg == "--modules-dir") && i + 1 < argc) {
            modulesDirs.append(QString::fromUtf8(argv[i + 1]));
            ++i;
        }
    }

    return Daemon::start(argc, argv, modulesDirs);
}

static int runInlineMode(int argc, char* argv[])
{
    CoreArgs args = parseCommandLineArgs(argc, argv);
    if (!args.valid)
        return 1;

    QCoreApplication app(argc, argv);
    app.setApplicationName("logoscore");
    app.setApplicationVersion("1.0");

    logos_core_init(argc, argv);

    for (const std::string& dir : args.modulesDirs) {
        logos_core_add_plugins_dir(dir.c_str());
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
    // Check for verbose flag before any Qt operations
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            g_verbose = true;
            break;
        }
    }

    qInstallMessageHandler(messageHandler);

    Mode mode = detectMode(argc, argv);

    switch (mode) {
    case Mode::Help:
        printHelp();
        return 0;

    case Mode::Version:
        std::cout << "logoscore version 1.0" << std::endl;
        return 0;

    case Mode::Daemon:
    {
        QCoreApplication app(argc, argv);
        app.setApplicationName("logoscore");
        app.setApplicationVersion("1.0");
        return runDaemonMode(argc, argv);
    }

    case Mode::Client:
    {
        QCoreApplication app(argc, argv);
        app.setApplicationName("logoscore");
        app.setApplicationVersion("1.0");
        return runClientMode(argc, argv);
    }

    case Mode::Inline:
        return runInlineMode(argc, argv);
    }

    return 1;
}
