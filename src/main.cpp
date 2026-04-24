#include <CLI/CLI.hpp>
#include <QCoreApplication>
#include <QDebug>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "config.h"
#include "paths.h"
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
                         bool quitOnFinish,
                         const std::string& persistencePath)
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
        const char* resolved = ec ? dir.c_str() : absDir.c_str();
        qDebug() << "Added plugins directory:" << resolved;
        logos_core_add_plugins_dir(resolved);
    }

    std::string bundledDir = paths::bundledModulesDir();
    if (!bundledDir.empty()) {
        logos_core_add_plugins_dir(bundledDir.c_str());
        qDebug() << "Added bundled modules directory:" << bundledDir.c_str();
    }

    // Set persistence base path (user-specified or default)
    std::string resolvedPersistence = persistencePath;
    if (resolvedPersistence.empty()) {
        resolvedPersistence = Config::configDir().toStdString() + "/data";
    }
    logos_core_set_persistence_base_path(resolvedPersistence.c_str());

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

    return QCoreApplication::exec();
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

    std::string persistencePath;
    app.add_option("--persistence-path", persistencePath,
        "Base directory for module instance persistence (default: ~/.logoscore/data)");

    // Override the config dir (daemon.json, config.json, data/) so parallel
    // logoscore instances can run side-by-side. Client commands must be
    // invoked with the same --config-dir as the daemon they target.
    std::string configDirStr;
    app.add_option("--config-dir", configDirStr,
        "Override config directory (default: ~/.logoscore; also LOGOSCORE_CONFIG_DIR)");

    // ── Transport flags (daemon side) ────────────────────────────────────────
    // --transport is repeatable: each appearance enables another listener.
    // `local` is always implicitly present, so the CLI on the same host
    // keeps working without any extra flags. Adding --transport=tcp or
    // --transport=tcp_ssl opens an additional listener at the given
    // host/port for remote clients (e.g. daemon in docker, CLI on host).
    std::vector<std::string> transportFlags;
    app.add_option("--transport", transportFlags,
        "Enable a transport for core_service: local | tcp | tcp_ssl (repeatable)");

    std::string tcpHost = "127.0.0.1";
    uint16_t    tcpPort = 0;
    std::string tcpCodec = "json";
    app.add_option("--tcp-host", tcpHost, "TCP bind address (default 127.0.0.1)");
    app.add_option("--tcp-port", tcpPort, "TCP port (0 = auto-assign)");
    app.add_option("--tcp-codec", tcpCodec,
        "Wire codec for the TCP listener: json (default) | cbor");

    std::string tcpSslHost = "127.0.0.1";
    uint16_t    tcpSslPort = 0;
    std::string tcpSslCodec = "json";
    std::string sslCaFile, sslCertFile, sslKeyFile;
    app.add_option("--tcp-ssl-host", tcpSslHost, "TCP+SSL bind address");
    app.add_option("--tcp-ssl-port", tcpSslPort, "TCP+SSL port (0 = auto-assign)");
    app.add_option("--tcp-ssl-codec", tcpSslCodec,
        "Wire codec for the TCP+SSL listener: json (default) | cbor");
    app.add_option("--ssl-ca",   sslCaFile,   "CA file (TCP+SSL)");
    app.add_option("--ssl-cert", sslCertFile, "Server cert (TCP+SSL)");
    app.add_option("--ssl-key",  sslKeyFile,  "Server private key (TCP+SSL)");

    // ── Transport flags (client side) ────────────────────────────────────────
    std::string clientTransport;  // empty = prefer local
    app.add_option("--client-transport", clientTransport,
        "Pick one of the daemon's advertised transports: local | tcp | tcp_ssl");
    std::string clientTcpHost;
    app.add_option("--client-tcp-host", clientTcpHost,
        "Override the daemon's advertised host (e.g. 'localhost' when daemon bound 0.0.0.0 in docker)");
    uint16_t clientTcpPort = 0;
    app.add_option("--client-tcp-port", clientTcpPort,
        "Override the daemon's advertised port (useful when port-forwarding or NAT changes the reachable port)");
    bool clientNoVerifyPeer = false;
    app.add_flag("--no-verify-peer", clientNoVerifyPeer,
        "Disable TLS peer verification (dev only)");
    std::string clientCodec;  // empty = accept whatever the daemon advertised
    app.add_option("--client-codec", clientCodec,
        "Require a specific wire codec (json | cbor); if the daemon advertised "
        "a different codec for the picked transport, connect fails.");

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

    // Token-management subcommands. These operate directly on the config
    // dir (no daemon connection required), so they can be used offline to
    // prepare client credentials before the daemon even starts.
    auto* issueTokenSub    = app.add_subcommand("issue-token",
        "Issue a new client token under --name NAME");
    auto* revokeTokenSub   = app.add_subcommand("revoke-token",
        "Revoke a previously-issued client token");
    auto* listTokensSub    = app.add_subcommand("list-tokens",
        "List the names of issued client tokens");

    // Allow extras on all client subcommands so their positional args and
    // command-specific flags pass through to the Command objects unchanged
    for (auto* sub : {statusSub, loadModuleSub, unloadModuleSub, reloadModuleSub,
                      listModulesSub, moduleInfoSub, infoSub, callSub, moduleSub,
                      watchSub, statsSub, stopSub,
                      issueTokenSub, revokeTokenSub, listTokensSub}) {
        sub->allow_extras();
    }

    app.require_subcommand(0, 1);  // 0 or 1 subcommand

    // ── Parse ────────────────────────────────────────────────────────────────
    CLI11_PARSE(app, argc, argv);

    qInstallMessageHandler(messageHandler);

    // Apply --config-dir (if passed) before any Config::* call so the daemon,
    // client, connection_file, and any forked logos_host all see the same
    // config dir. Also mirror into the env var so child processes inherit it.
    if (!configDirStr.empty()) {
        std::error_code ec;
        const std::filesystem::path absCfgPath =
            std::filesystem::absolute(configDirStr, ec);
        if (ec) {
            std::cerr << "Error: failed to resolve --config-dir '" << configDirStr
                      << "': " << ec.message() << std::endl;
            return 1;
        }
        std::filesystem::create_directories(absCfgPath, ec);
        if (ec) {
            std::cerr << "Error: failed to create --config-dir '" << absCfgPath.string()
                      << "': " << ec.message() << std::endl;
            return 1;
        }
        const QString absCfg = QString::fromStdString(absCfgPath.string());
        Config::setConfigDir(absCfg);
        qputenv("LOGOSCORE_CONFIG_DIR", absCfg.toUtf8());
    }

    // ── Daemon mode ──────────────────────────────────────────────────────────
    if (daemonFlag || daemonSub->parsed()) {
        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion("1.0");

        // Translate --transport flags into TransportInfo records. Anything
        // not explicitly requested is dropped (local is implicitly always
        // on, set up inside Daemon::start).
        auto validateCodec = [](const std::string& c) {
            return c == "json" || c == "cbor";
        };

        std::vector<TransportInfo> transportInfos;
        for (const auto& proto : transportFlags) {
            TransportInfo t;
            t.protocol = proto;
            if (proto == "tcp") {
                t.host = tcpHost; t.port = tcpPort;
                if (!validateCodec(tcpCodec)) {
                    std::cerr << "Error: --tcp-codec must be 'json' or 'cbor', got: "
                              << tcpCodec << std::endl;
                    return 1;
                }
                t.codec = tcpCodec;
            } else if (proto == "tcp_ssl") {
                t.host = tcpSslHost; t.port = tcpSslPort;
                t.caFile = sslCaFile;
                t.verifyPeer = true;
                if (!validateCodec(tcpSslCodec)) {
                    std::cerr << "Error: --tcp-ssl-codec must be 'json' or 'cbor', got: "
                              << tcpSslCodec << std::endl;
                    return 1;
                }
                t.codec = tcpSslCodec;
                if (sslCertFile.empty() || sslKeyFile.empty()) {
                    std::cerr << "Error: --transport=tcp_ssl requires --ssl-cert and --ssl-key"
                              << std::endl;
                    return 1;
                }
                t.certFile = sslCertFile;
                t.keyFile  = sslKeyFile;
            } else if (proto != "local") {
                std::cerr << "Error: unknown --transport value: " << proto
                          << " (expected local | tcp | tcp_ssl)" << std::endl;
                return 1;
            }
            transportInfos.push_back(std::move(t));
        }

        return Daemon::start(argc, argv, modulesDirs, persistencePath, transportInfos);
    }

    // Propagate client-side transport selection to RpcClient via env vars
    // so subcommand dispatch (below) doesn't need new plumbing.
    if (!clientTransport.empty())
        qputenv("LOGOSCORE_CLIENT_TRANSPORT", clientTransport.c_str());
    if (!clientTcpHost.empty())
        qputenv("LOGOSCORE_CLIENT_TCP_HOST", clientTcpHost.c_str());
    if (clientTcpPort != 0)
        qputenv("LOGOSCORE_CLIENT_TCP_PORT",
                QByteArray::number(static_cast<int>(clientTcpPort)));
    if (clientNoVerifyPeer)
        qputenv("LOGOSCORE_CLIENT_NO_VERIFY_PEER", "1");
    if (!clientCodec.empty())
        qputenv("LOGOSCORE_CLIENT_CODEC", clientCodec.c_str());

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
        {issueTokenSub,  "issue-token"},
        {revokeTokenSub, "revoke-token"},
        {listTokensSub,  "list-tokens"},
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
        return runInlineMode(argc, argv, modulesDirs, loadModulesStr, callStrs, quitOnFinish, persistencePath);
    }

    // ── No mode detected — show help ─────────────────────────────────────────
    std::cout << app.help() << std::endl;
    return 0;
}
