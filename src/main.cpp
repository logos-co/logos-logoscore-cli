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
#include "daemon/daemon_state.h"
#include "client/client_state.h"
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

// Pre-scan argv for `--config-dir` so we can apply the override (and
// resolve the corresponding `<configDir>/config.json` path) *before*
// CLI11 parses anything else. Returns the override path or empty if
// not present. Recognises both `--config-dir X` (two tokens) and
// `--config-dir=X` (single token); mirrors what CLI11 would parse.
static std::string preScanConfigDir(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config-dir" && i + 1 < argc) return argv[i + 1];
        const std::string prefix = "--config-dir=";
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return {};
}

int main(int argc, char *argv[])
{
    // Pre-scan argv for `--config-dir` so the override applies before
    // any Config::* call. The CLI11 parse below picks the same flag up
    // again and re-applies it, but we need it earlier than that —
    // anything that touches Config::configDir during option parsing
    // (e.g. logging) would otherwise key off the wrong directory.
    {
        std::string preDir = preScanConfigDir(argc, argv);
        if (!preDir.empty()) {
            std::error_code ec;
            const std::filesystem::path absCfgPath =
                std::filesystem::absolute(preDir, ec);
            if (!ec)
                Config::setConfigDir(QString::fromStdString(absCfgPath.string()));
        }
    }

    // The config tree splits by lifetime under <configDir>/:
    //   daemon/config.json   — operator preferences (writes only on --persist-config)
    //   daemon/state.json    — live runtime state (created at boot, removed at shutdown)
    //   daemon/tokens.json   — hashed-at-rest accepted tokens (survives restarts)
    //   client/config.json   — client dial spec (writes only on --persist-config)
    // CLI flags merge over disk via per-flag Option::count() detection.

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
    auto* modulesDirOpt = app.add_option("-m,--modules-dir", modulesDirs,
        "Module search directory (repeatable)");

    // Inline mode options
    std::string loadModulesStr;
    auto* loadModulesOpt = app.add_option("-l,--load-modules", loadModulesStr,
        "Comma-separated modules to load");

    std::vector<std::string> callStrs;
    app.add_option("-c,--call", callStrs, "Call module.method(args) (repeatable)");

    bool quitOnFinish = false;
    app.add_flag("--quit-on-finish", quitOnFinish, "Exit after calls complete");

    std::string persistencePath;
    auto* persistencePathOpt = app.add_option("--persistence-path", persistencePath,
        "Base directory for module instance persistence (default: ~/.logoscore/data)");

    // --persist-config: write the merged (defaults < config.json < CLI)
    // result to disk. Without it, CLI flags affect the running process
    // only; with it, the next no-flag launch reproduces the same
    // behavior. Applies symmetrically to daemon (daemon/config.json) and
    // client (client/config.json) modes.
    bool persistConfig = false;
    app.add_flag("--persist-config", persistConfig,
        "Write the merged config to config.json so the next launch reproduces these flags");

    // Override the config dir (daemon/{config,state,tokens}.json,
    // client/config.json, data/) so parallel logoscore instances can
    // run side-by-side. Client commands must be invoked with the
    // same --config-dir as the daemon they target.
    std::string configDirStr;
    app.add_option("--config-dir", configDirStr,
        "Override config directory (default: ~/.logoscore; also LOGOSCORE_CONFIG_DIR)");

    // ── Transport flags (daemon side) ────────────────────────────────────────
    // Per-module transport configuration. Each `--module-transport`
    // adds one listener to the named module. Format:
    //
    //     NAME=PROTOCOL[,k=v[,k=v...]]
    //
    // PROTOCOL is `local`, `tcp`, or `tcp_ssl`. Recognized k=v pairs:
    //
    //     host         (tcp / tcp_ssl)
    //     port         (tcp / tcp_ssl; 0 = auto-allocate ephemeral)
    //     codec        (tcp / tcp_ssl; "json" default | "cbor")
    //     ca           (tcp_ssl; CA cert path for client verification)
    //     cert,key     (tcp_ssl; server cert + key paths)
    //     verify_peer  (tcp_ssl; "true"|"false", default true)
    //
    // Repeatable. Each module gets its own list — there is no
    // "core_service is the source, capability_module inherits" magic
    // any more. Operators are expected to configure each module
    // explicitly, matching the on-disk shape of daemon/state.json's
    // resolved.modules block.
    //
    // Default when omitted: each well-known module
    // (`core_service`, `capability_module`) gets a single `local`
    // listener.
    //
    // Examples:
    //     logoscore -D \
    //         --module-transport core_service=local \
    //         --module-transport core_service=tcp,host=0.0.0.0,port=6000,codec=json \
    //         --module-transport capability_module=local \
    //         --module-transport capability_module=tcp,host=0.0.0.0,port=6001,codec=json
    std::vector<std::string> moduleTransportFlags;
    auto* moduleTransportOpt = app.add_option("--module-transport", moduleTransportFlags,
        "Configure a module's transport: NAME=PROTOCOL[,k=v...] (repeatable)");

    // Plaintext-TCP safety net: refuse to bind plaintext `tcp` on a
    // non-loopback host unless this flag is set. Without it, a daemon
    // configured with `--module-transport core_service=tcp,host=0.0.0.0`
    // would put tokens on the wire in cleartext on every RPC. The
    // escape hatch exists for trusted-network test setups; production
    // use should pass tcp_ssl or wrap with a TLS terminator.
    bool insecureTcp = false;
    auto* insecureTcpOpt = app.add_flag("--insecure-tcp", insecureTcp,
        "Allow plaintext tcp on non-loopback hosts (tokens travel cleartext)");

    // ── Transport flags (client side) ────────────────────────────────────────
    std::string clientTransport;  // empty = prefer local
    auto* clientTransportOpt = app.add_option("--client-transport", clientTransport,
        "Pick one of the daemon's advertised transports: local | tcp | tcp_ssl");
    std::string clientTcpHost;
    auto* clientTcpHostOpt = app.add_option("--client-tcp-host", clientTcpHost,
        "Override the daemon's advertised host (e.g. 'localhost' when daemon bound 0.0.0.0 in docker)");
    uint16_t clientTcpPort = 0;
    auto* clientTcpPortOpt = app.add_option("--client-tcp-port", clientTcpPort,
        "Override the daemon's advertised port (useful when port-forwarding or NAT changes the reachable port)");
    bool clientNoVerifyPeer = false;
    auto* clientNoVerifyPeerOpt = app.add_flag("--no-verify-peer", clientNoVerifyPeer,
        "Disable TLS peer verification (dev only)");
    std::string clientCodec;  // empty = accept whatever the daemon advertised
    auto* clientCodecOpt = app.add_option("--client-codec", clientCodec,
        "Require a specific wire codec (json | cbor); if the daemon advertised "
        "a different codec for the picked transport, connect fails.");
    std::string clientTokenFile;
    auto* clientTokenFileOpt = app.add_option("--token-file", clientTokenFile,
        "Filename inside client/ to use for authentication (must already exist; "
        "no copy semantics)");
    std::string clientSslCa;
    auto* clientSslCaOpt = app.add_option("--ssl-ca", clientSslCa,
        "CA cert path used to verify the daemon's TLS chain (tcp_ssl only)");

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

        // Plaintext-TCP guard: a `tcp` listener on a non-loopback host
        // sends tokens in cleartext. Refuse to start unless the
        // operator explicitly opted in.
        auto isLoopback = [](const std::string& h) {
            return h == "127.0.0.1" || h == "::1" || h == "localhost";
        };
        auto validateCodec = [](const std::string& c) {
            return c == "json" || c == "cbor";
        };

        // Parse --module-transport NAME=PROTOCOL[,k=v...] flags into
        // a per-module map. Each flag adds one listener to the named
        // module. There is deliberately no implicit core_service /
        // capability_module relationship — each module's transport
        // list is built solely from its own flags, then defaulted
        // to a single LocalSocket entry below for the well-known
        // modules if the operator didn't configure them.
        std::map<std::string, std::vector<TransportInfo>> moduleTransportsMap;
        for (const auto& spec : moduleTransportFlags) {
            const auto eq = spec.find('=');
            if (eq == std::string::npos || eq == 0 || eq == spec.size() - 1) {
                std::cerr << "Error: --module-transport expects "
                          << "'NAME=PROTOCOL[,k=v...]', got: '" << spec
                          << "'" << std::endl;
                return 1;
            }
            const std::string moduleName = spec.substr(0, eq);
            const std::string body = spec.substr(eq + 1);

            // Split body on commas. The first part is the protocol;
            // the rest are key=value pairs. Splitting comma-separated
            // is safe — neither host names nor port numbers nor codec
            // names contain commas; cert paths on POSIX don't either.
            std::vector<std::string> parts;
            for (size_t i = 0; i < body.size(); ) {
                auto comma = body.find(',', i);
                parts.push_back(body.substr(i, comma == std::string::npos
                                                  ? std::string::npos
                                                  : comma - i));
                if (comma == std::string::npos) break;
                i = comma + 1;
            }
            if (parts.empty() || parts[0].empty()) {
                std::cerr << "Error: --module-transport '" << spec
                          << "' missing protocol" << std::endl;
                return 1;
            }

            TransportInfo t;
            t.protocol = parts[0];

            // Defaults for tcp / tcp_ssl when the operator omits
            // them. host="127.0.0.1" matches the daemon-side bind
            // convention used by older flags and keeps a bare
            // `--module-transport core_service=tcp` working out of
            // the box on a single host.
            if (t.protocol == "tcp" || t.protocol == "tcp_ssl") {
                t.host = "127.0.0.1";
                t.codec = "json";
                t.verifyPeer = true;
            }

            for (size_t i = 1; i < parts.size(); ++i) {
                const auto& kv = parts[i];
                const auto kvSep = kv.find('=');
                if (kvSep == std::string::npos) {
                    std::cerr << "Error: --module-transport '" << spec
                              << "' has malformed kv pair '" << kv
                              << "' (expected k=v)" << std::endl;
                    return 1;
                }
                const std::string k = kv.substr(0, kvSep);
                const std::string v = kv.substr(kvSep + 1);
                if      (k == "host")  t.host = v;
                else if (k == "codec") t.codec = v;
                else if (k == "ca")    t.caFile = v;
                else if (k == "cert")  t.certFile = v;
                else if (k == "key")   t.keyFile = v;
                else if (k == "verify_peer") t.verifyPeer = (v == "true" || v == "1");
                else if (k == "port") {
                    int parsedPort = 0;
                    try { parsedPort = std::stoi(v); }
                    catch (...) {
                        std::cerr << "Error: --module-transport port '" << v
                                  << "' is not a valid integer" << std::endl;
                        return 1;
                    }
                    if (parsedPort < 0 || parsedPort > 0xFFFF) {
                        std::cerr << "Error: --module-transport port '" << v
                                  << "' must be in [0, 65535]" << std::endl;
                        return 1;
                    }
                    t.port = static_cast<uint16_t>(parsedPort);
                } else {
                    std::cerr << "Error: --module-transport unknown key '"
                              << k << "' in '" << spec << "'" << std::endl;
                    return 1;
                }
            }

            // Per-protocol validation.
            if (t.protocol == "local") {
                // host/port/codec/cert ignored for local.
            } else if (t.protocol == "tcp") {
                if (!validateCodec(t.codec)) {
                    std::cerr << "Error: --module-transport tcp codec '"
                              << t.codec << "' must be 'json' or 'cbor'"
                              << std::endl;
                    return 1;
                }
                // Plaintext-TCP guard runs post-merge below so disk-fed
                // listeners are also covered.
            } else if (t.protocol == "tcp_ssl") {
                if (!validateCodec(t.codec)) {
                    std::cerr << "Error: --module-transport tcp_ssl codec '"
                              << t.codec << "' must be 'json' or 'cbor'"
                              << std::endl;
                    return 1;
                }
                if (t.certFile.empty() || t.keyFile.empty()) {
                    std::cerr << "Error: --module-transport tcp_ssl for '"
                              << moduleName << "' requires cert= and key="
                              << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: --module-transport unknown protocol '"
                          << t.protocol << "' (expected local | tcp | tcp_ssl)"
                          << std::endl;
                return 1;
            }

            moduleTransportsMap[moduleName].push_back(std::move(t));
        }

        // Per-flag merge: load disk config (if any), then layer CLI
        // overrides on top — but only for flags the operator
        // explicitly passed. CLI11's Option::count() is the only
        // accurate signal: a default-valued local var is
        // indistinguishable from an explicit `--load-modules ""`
        // without it. Anything not touched by either CLI or disk
        // falls through to defaults.
        DaemonConfig mergedCfg;
        std::string  configSource = "defaults";

        if (auto disk = DaemonConfigFile::read()) {
            mergedCfg = *disk;
            configSource = "config.json";
        }

        const bool anyCliFlag = (modulesDirOpt->count()      > 0)
                             || (loadModulesOpt->count()     > 0)
                             || (persistencePathOpt->count() > 0)
                             || (moduleTransportOpt->count() > 0)
                             || (insecureTcpOpt->count()     > 0);
        if (anyCliFlag) configSource = "cli";

        if (modulesDirOpt->count() > 0)      mergedCfg.modulesDirs     = modulesDirs;
        if (loadModulesOpt->count() > 0)     mergedCfg.loadModules     = loadModulesStr;
        if (persistencePathOpt->count() > 0) mergedCfg.persistencePath = persistencePath;
        if (insecureTcpOpt->count() > 0)     mergedCfg.insecureTcp     = insecureTcp;
        // --module-transport replaces the disk's modules wholesale
        // when the operator passes any. There's no per-module merge:
        // mixing operator intent with stale disk entries leads to
        // surprising behavior (a flag that disabled a listener on
        // disk would silently re-enable it). Either operator-specified
        // or disk-specified — never a hybrid.
        if (moduleTransportOpt->count() > 0) mergedCfg.modules = moduleTransportsMap;

        // Default the well-known modules to LocalSocket-only when neither
        // the operator nor disk-config supplied them. Without this, a
        // bare `logoscore -D` would start a daemon with no listeners and
        // clients would have nothing to dial.
        for (const std::string& wellKnown : {"core_service", "capability_module"}) {
            if (mergedCfg.modules.find(wellKnown) == mergedCfg.modules.end()) {
                TransportInfo t;
                t.protocol = "local";
                mergedCfg.modules[wellKnown].push_back(std::move(t));
            }
        }

        // Plaintext-TCP guard, post-merge: refuse to bind plaintext
        // tcp on a non-loopback host unless `insecure_tcp` is enabled
        // (whether by --insecure-tcp on the CLI or by config.json).
        // Iterating the merged map means a disk-supplied plaintext
        // listener gets the same scrutiny as a CLI-supplied one — the
        // operator can't bypass the guard by stashing the combo in
        // config.json.
        for (const auto& [moduleName, transports] : mergedCfg.modules) {
            for (const auto& t : transports) {
                if (t.protocol != "tcp") continue;
                if (isLoopback(t.host)) continue;
                if (mergedCfg.insecureTcp) continue;
                std::cerr << "Error: module '" << moduleName
                          << "' binds plaintext tcp on non-loopback host '"
                          << t.host << "'. Use protocol=tcp_ssl, or pass "
                          << "--insecure-tcp if you really mean it."
                          << std::endl;
                return 1;
            }
        }

        return Daemon::start(argc, argv, mergedCfg, configSource, persistConfig);
    }

    // ── Client-side per-flag merge ───────────────────────────────────────────
    // Same precedence as the daemon side: defaults < client/config.json
    // < CLI args. CLI11's Option::count() drives per-flag override
    // detection. If the operator passed any client-config flag, the
    // merged result takes effect for this run; if `--persist-config`
    // is also passed, the merged result is written back to
    // client/config.json so subsequent no-flag launches reproduce it.
    {
        const bool anyClientCfgFlag = (clientTransportOpt->count()    > 0)
                                   || (clientTcpHostOpt->count()      > 0)
                                   || (clientTcpPortOpt->count()      > 0)
                                   || (clientNoVerifyPeerOpt->count() > 0)
                                   || (clientCodecOpt->count()        > 0)
                                   || (clientTokenFileOpt->count()    > 0)
                                   || (clientSslCaOpt->count()        > 0);

        if (anyClientCfgFlag || persistConfig) {
            ClientState merged = ClientStateFile::read();  // disk (or empty)

            // The transport-shape overrides apply to BOTH dialed
            // modules (core_service and capability_module) since
            // both go through the same daemon endpoint. An operator
            // who needs per-module divergence has to hand-edit
            // client/config.json — keeping the CLI surface small.
            auto applyToModule = [&](const std::string& moduleName) {
                ClientModuleTransport& t = merged.daemon[moduleName];
                if (clientTransportOpt->count() > 0) t.protocol = clientTransport;
                if (clientTcpHostOpt->count()   > 0) t.host     = clientTcpHost;
                if (clientTcpPortOpt->count()   > 0) t.port     = clientTcpPort;
                if (clientCodecOpt->count()     > 0) t.codec    = clientCodec;
                if (clientNoVerifyPeerOpt->count() > 0) t.verifyPeer = !clientNoVerifyPeer;
                if (clientSslCaOpt->count()     > 0) t.caFile   = clientSslCa;
                // Default protocol when this module is being
                // freshly added by CLI flags (i.e. nothing on disk
                // and the operator didn't pick a transport).
                if (t.protocol.empty()) t.protocol = "local";
            };
            applyToModule("core_service");
            applyToModule("capability_module");

            if (clientTokenFileOpt->count() > 0) {
                merged.tokenFile = clientTokenFile;
                // Refuse to start if the named raw-token file isn't
                // already under client/. No copy semantics — the
                // operator is expected to scp the daemon-side
                // tokens/<name>.json into place themselves.
                std::error_code ec;
                if (!std::filesystem::exists(
                        Config::clientTokenPath(QString::fromStdString(clientTokenFile))
                            .toStdString(), ec)) {
                    std::cerr << "Error: --token-file '" << clientTokenFile
                              << "' does not exist at "
                              << Config::clientDir().toStdString() << "/" << clientTokenFile
                              << ". Copy it from the daemon's daemon/tokens/ dir first."
                              << std::endl;
                    return 1;
                }
            }

            // Stamp the schema version in case the merge built it
            // up from defaults — the on-disk path needs it for the
            // version check in ClientStateFile::read.
            merged.schemaVersion = kClientStateSchemaVersion;
            // `fileOk` is the "this is usable for dialing" bit;
            // RpcClient::connect checks it. The merge guarantees
            // every run has at least one daemon entry, so fileOk
            // is true iff a token_file is also set.
            merged.fileOk = !merged.daemon.empty() && !merged.tokenFile.empty();

            // Inject merged state into ClientStateFile so the
            // override applies for this run regardless of disk state.
            ClientStateFile::setOverride(merged);

            if (persistConfig) {
                if (ClientStateFile::write(merged)) {
                    fprintf(stdout, "Persisted client config: %s\n",
                            ClientStateFile::filePath().c_str());
                } else {
                    fprintf(stderr, "Warning: failed to persist client config to %s\n",
                            ClientStateFile::filePath().c_str());
                }
            }
        }
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
