#include <CLI/CLI.hpp>
#include <QCoreApplication>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "config.h"
#include "paths.h"
#include "daemon/daemon.h"
#include "daemon/daemon_state.h"
#include "client/client_state.h"
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"
#include "logos_core.h"
#include "version_info.h"

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

// Resolve the --access-policy argument: inline JSON if it starts with
// '{', otherwise a path to read from disk. Parse-checks the result and
// returns nullopt (after a stderr diagnostic) on any error.
static std::optional<std::string> resolveAccessPolicy(const std::string& arg)
{
    std::string content;
    std::string source;  // for diagnostics

    auto firstNonSpace = std::find_if(arg.begin(), arg.end(),
        [](unsigned char c) { return !std::isspace(c); });
    const bool looksInline = (firstNonSpace != arg.end() && *firstNonSpace == '{');

    if (looksInline) {
        content = arg;
        source = "inline --access-policy JSON";
    } else {
        std::ifstream ifs(arg, std::ios::binary);
        if (!ifs) {
            std::cerr << "Error: --access-policy file '" << arg
                      << "' could not be opened." << std::endl;
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << ifs.rdbuf();
        content = ss.str();
        source = "--access-policy file '" + arg + "'";
    }

    // Parse-check only; schema enforcement is the runtime's job.
    try {
        (void)nlohmann::json::parse(content);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << source << " is not valid JSON: "
                  << e.what() << std::endl;
        return std::nullopt;
    }

    return content;
}

// Collapse the two-token group verbs into the single tokens CLI11 has
// subcommands for, before any parsing happens:
//
//     daemon config ...  ->  daemon-config ...
//     client config ...  ->  client-config ...
//     daemon start   ...  ->  daemon ...
//     daemon stop    ...  ->  stop ...
//     daemon status  ...  ->  status ...
//
// Doing this in argv rather than with nested CLI11 subcommands avoids a
// collision with daemonSub->fallthrough(): a nested subcommand's unmatched
// arguments fall through to the parent and then to the top level, which
// rejects them ("The following argument was not expected: show").
static std::vector<std::string> normalizeGroupVerbs(int argc, char* argv[])
{
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        const std::string next = (i + 1 < argc) ? argv[i + 1] : std::string{};
        if (a == "daemon" || a == "client") {
            if (next == "config") { out.push_back(a + "-config"); ++i; continue; }
            if (a == "daemon" && next == "start")  { out.push_back("daemon"); ++i; continue; }
            if (a == "daemon" && next == "stop")   { out.push_back("stop");   ++i; continue; }
            if (a == "daemon" && next == "status") { out.push_back("status"); ++i; continue; }
        }
        out.push_back(a);
    }
    return out;
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
                Config::setConfigDir(absCfgPath.string());
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
    app.set_version_flag("--version", logoscore_version::versionString());
    app.set_help_flag("-h,--help", "Show this help");

    // Global flags
    app.add_flag("-v,--verbose", g_verbose, "Show debug logs");

    // Client global flags (defined at app level so they work before subcommands;
    // also extracted from subcommand remaining() for placement after subcommands)
    bool jsonMode = false;
    app.add_flag("-j,--json", jsonMode, "Force JSON output");

    bool humanMode = false;
    app.add_flag("--no-json,--human", humanMode,
                 "Force human-readable output even when piped");

    bool quiet = false;
    app.add_flag("-q,--quiet", quiet, "Suppress non-essential output");

    // Daemon flag (-D as shorthand for daemon subcommand)
    bool daemonFlag = false;
    app.add_flag("-D", daemonFlag, "Start the daemon process");

    // Everything that used to be a flag here -- module directories, the
    // persistence path, per-module transports, the access policy and group,
    // the plaintext-TCP opt-in, and the seven client dial-spec flags -- now
    // lives in the session's YAML documents, written with
    // `daemon config set` / `client config set`. See src/yaml_json.h for why
    // the split is YAML for humans, JSON for machines.
    //
    // Configuration is never passed alongside an unrelated command, so
    // `daemon start` and every client command take the session exactly as it
    // is on disk. That removes the whole defaults < config.json < CLI merge
    // layer, along with the ad-hoc `NAME=PROTOCOL[,k=v...]` grammar that
    // existed only to squeeze a nested structure through a flag.
    //
    // --config-dir is the one survivor, and it is not configuration: it
    // selects *which* session to act on, so it cannot itself live inside one.
    std::string configDirStr;
    app.add_option("--config-dir", configDirStr,
        "Session directory to use (default: ~/.logoscore; also LOGOSCORE_CONFIG_DIR). "
        "Holds this session's config, modules, plugins, keyring and data.");

    // ── Client subcommands ───────────────────────────────────────────────────
    // All client subcommands use allow_extras() so their positional args and
    // command-specific flags (--loaded, --event) are captured in remaining().
    // Global flags (--json, --quiet) mixed in after the subcommand are also
    // captured and extracted before dispatching to the command object.

    // The `daemon` group. A bare `daemon` (or -D) still starts the runtime;
    // `daemon start|stop|status` are the explicit spellings, and
    // `daemon config show|set` operates on the session's config file without
    // needing a daemon at all.
    auto* daemonSub  = app.add_subcommand("daemon",
        "Start the daemon, or manage it: start | stop | status | config show|set FILE");
    daemonSub->fallthrough();  // -m, -v after "daemon" fall through to parent
    // The group verbs are normalized in argv before parsing (see
    // normalizeGroupVerbs), so `daemon config` arrives here as the single
    // token `daemon-config`. Declaring them flat keeps them clear of
    // daemonSub->fallthrough(), which otherwise pushes a nested
    // subcommand's extras up to the top level where they are rejected.
    auto* daemonConfigSub = app.add_subcommand("daemon-config",
        "Show or replace the daemon configuration: show | set FILE");
    auto* clientConfigSub = app.add_subcommand("client-config",
        "Show or replace the client dial spec: show | set FILE");
    daemonConfigSub->allow_extras();
    clientConfigSub->allow_extras();

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

    // Package management. These are client commands like any other: they RPC
    // into the bundled package_manager / package_downloader modules, so they
    // need a running daemon.
    auto* packageSub       = app.add_subcommand("package",
        "Install, remove and inspect packages");
    auto* catalogSub       = app.add_subcommand("catalog",
        "Manage the package catalogs this session pulls from");
    auto* keySub           = app.add_subcommand("key",
        "Manage trusted package-signing keys");
    // Top-level shortcuts for the two package verbs that have no
    // runtime-module meaning, so they cannot be confused with `module ...`.
    auto* installSub       = app.add_subcommand("install",
        "Alias for 'package install'");
    auto* searchSub        = app.add_subcommand("search",
        "Alias for 'package search'");

    // Allow extras on all client subcommands so their positional args and
    // command-specific flags pass through to the Command objects unchanged
    for (auto* sub : {statusSub, loadModuleSub, unloadModuleSub, reloadModuleSub,
                      listModulesSub, moduleInfoSub, infoSub, callSub, moduleSub,
                      watchSub, statsSub, stopSub,
                      issueTokenSub, revokeTokenSub, listTokensSub,
                      packageSub, catalogSub, keySub, installSub, searchSub}) {
        sub->allow_extras();
    }

    app.require_subcommand(0, 1);  // 0 or 1 subcommand

    // ── Parse ────────────────────────────────────────────────────────────────
    // Parse the normalized argv (group verbs collapsed). The original argc/argv
    // are still handed to QCoreApplication below, which only cares about Qt's
    // own switches.
    std::vector<std::string> normalized = normalizeGroupVerbs(argc, argv);
    std::vector<char*> normalizedArgv;
    normalizedArgv.reserve(normalized.size());
    for (auto& a : normalized) normalizedArgv.push_back(a.data());
    const int normalizedArgc = static_cast<int>(normalizedArgv.size());
    CLI11_PARSE(app, normalizedArgc, normalizedArgv.data());

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
        Config::setConfigDir(absCfgPath.string());
        setenv("LOGOSCORE_CONFIG_DIR", absCfgPath.string().c_str(), 1);
    }

    // ── `daemon <verb>` / `client <verb>` routing ────────────────────────────
    // Management verbs are handled before the daemon-mode branch below, so
    // `daemon config set` and `daemon stop` never boot a second runtime.
    {
        auto runManagementCommand = [&](const std::string& mapped,
                                        CLI::App* sub) -> int {
            std::vector<std::string> extras;
            for (const auto& r : sub->remaining()) {
                if (r == "--json" || r == "-j")               jsonMode = true;
                else if (r == "--no-json" || r == "--human")  humanMode = true;
                else if (r == "--quiet" || r == "-q")         quiet = true;
                else                                          extras.push_back(r);
            }
            QCoreApplication qapp(argc, argv);
            qapp.setApplicationName("logoscore");
            qapp.setApplicationVersion(QString::fromStdString(logoscore_version::version()));
            Output output(jsonMode);
            if (humanMode) output.setHumanMode(true);
            RpcClient rpcClient;
            auto cmd = createCommand(mapped, rpcClient, output);
            if (!cmd) {
                output.printError("INVALID_ARGS", "Unknown command: " + mapped);
                return 1;
            }
            return cmd->execute(extras);
        };

        if (daemonConfigSub->parsed()) return runManagementCommand("daemon-config", daemonConfigSub);
        if (clientConfigSub->parsed()) return runManagementCommand("client-config", clientConfigSub);
    }

    // ── Daemon mode ──────────────────────────────────────────────────────────
    if (daemonFlag || daemonSub->parsed()) {
        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion(QString::fromStdString(logoscore_version::version()));

        // Plaintext-TCP guard: a `tcp` listener on a non-loopback host
        // sends tokens in cleartext. Refuse to start unless the
        // operator explicitly opted in.
        auto isLoopback = [](const std::string& h) {
            return h == "127.0.0.1" || h == "::1" || h == "localhost";
        };
        auto validateCodec = [](const std::string& c) {
            return c == "json" || c == "cbor";
        };

        // Configuration comes solely from daemon/config.yaml. Absent or
        // unreadable means defaults, which is a working local-only daemon.
        DaemonConfig mergedCfg;
        std::string  configSource = "defaults";
        if (auto disk = DaemonConfigFile::read()) {
            mergedCfg = *disk;
            configSource = "config.yaml";
        }

        // Make sure the well-known modules at least *have* an entry,
        // so a bare `logoscore -D` (no transport flags) still boots
        // with listeners. The local-prepend below populates them.
        for (const std::string& wellKnown : {"core_service", "capability_module"}) {
            (void)mergedCfg.modules[wellKnown];  // default-construct empty
        }

        // Always make every configured module carry a LocalSocket
        // listener. Two reasons:
        //
        //  (1) Default modules (none operator-configured) need *some*
        //      listener — local is the cheapest, always-works choice.
        //
        //  (2) Even when the operator explicitly opts into TCP / TCP+SSL
        //      for a given module (e.g.
        //      `--module-transport core_service=tcp,...`,
        //      `--module-transport my_module=tcp,...`), a lot of
        //      intra-daemon code paths (capability_module's
        //      requestModule → core_service handshake; the daemon's
        //      own auto-`requestModule` flow inside LogosAPIClient;
        //      cross-module outbound `getClient(name)` calls) default
        //      to LocalSocket and have no plumbing to discover the
        //      operator's chosen TCP endpoint. Forcing a local listener
        //      alongside whatever else the operator named keeps those
        //      paths working without fan-out — the operator's TCP
        //      listener is the *additional* surface for outside clients.
        //
        // Order matters: we PREPEND local so it's the first entry in
        // each module's preference list, which means consumers that
        // pick "first transport" land on local. Operator-supplied
        // entries follow in the order they were typed.
        //
        // Applies to every module in the merged config, well-known or
        // user-configured — same logic, no special-casing.
        //
        // Normalization rule: at most one `local` entry per module,
        // always at index 0. If the operator typed `local` later in
        // the order (e.g. `--module-transport NAME=tcp,...
        // --module-transport NAME=local`) we MOVE that entry to the
        // front rather than leave it at index 1 and prepend a fresh
        // one — otherwise consumers that pick "first transport" would
        // still land on TCP, and we'd have two local entries to dedupe.
        for (auto& [moduleName, transports] : mergedCfg.modules) {
            (void)moduleName;
            auto localIt = std::find_if(transports.begin(), transports.end(),
                [](const TransportInfo& t) { return t.protocol == "local"; });
            TransportInfo localEntry;
            if (localIt != transports.end()) {
                localEntry = std::move(*localIt);
                transports.erase(localIt);
            } else {
                localEntry.protocol = "local";
            }
            transports.insert(transports.begin(), std::move(localEntry));
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

        return Daemon::start(argc, argv, mergedCfg, configSource, g_verbose);
    }

    // ── Client mode ──────────────────────────────────────────────────────────
    struct SubInfo { CLI::App* sub; std::string name; };
    std::vector<SubInfo> clientSubs = {
        {statusSub,       "status"},
        {loadModuleSub,   "load-module"},
        {unloadModuleSub, "unload-module"},
        {reloadModuleSub, "reload-module"},
        {listModulesSub,  "list-modules"},
        {moduleInfoSub,   "module-info"},
        {infoSub,         "info"},
        {callSub,         "call"},
        {moduleSub,       "module"},
        {watchSub,        "watch"},
        {statsSub,        "stats"},
        {stopSub,         "stop"},
        {issueTokenSub,   "issue-token"},
        {revokeTokenSub,  "revoke-token"},
        {listTokensSub,   "list-tokens"},
        {packageSub,      "package"},
        {catalogSub,      "catalog"},
        {keySub,          "key"},
        {installSub,      "install"},
        {searchSub,       "search"},
    };

    for (auto& [sub, name] : clientSubs) {
        if (!sub->parsed())
            continue;

        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logoscore");
        qapp.setApplicationVersion(QString::fromStdString(logoscore_version::version()));

        // Collect remaining args from the subcommand, extracting global flags
        // (global flags placed after the subcommand end up in remaining())
        std::vector<std::string> cmdArgs;

        for (const auto& r : sub->remaining()) {
            if (r == "--json" || r == "-j") {
                jsonMode = true;
            } else if (r == "--no-json" || r == "--human") {
                humanMode = true;
            } else if (r == "--quiet" || r == "-q") {
                quiet = true;
            } else {
                cmdArgs.push_back(r);
            }
        }

        // Top-level aliases are the same command with its subcommand
        // pre-supplied: `logoscore install X` is `logoscore package install X`.
        // Only verbs with no runtime-module meaning are aliased this way —
        // `ls`/`show`/`info` would be ambiguous between a package and a
        // loaded module, so they stay inside their groups.
        std::string commandName = name;
        if (name == "install" || name == "search") {
            cmdArgs.insert(cmdArgs.begin(), name);
            commandName = "package";
        }

        Output output(jsonMode);
        if (humanMode)
            output.setHumanMode(true);
        RpcClient rpcClient;

        auto cmd = createCommand(commandName, rpcClient, output);
        if (!cmd) {
            output.printError("INVALID_ARGS",
                              "Unknown command: " + name + ". Run 'logoscore --help' for usage.");
            return 1;
        }

        return cmd->execute(cmdArgs);
    }

    // ── No mode detected — show help ─────────────────────────────────────────
    std::cout << app.help() << std::endl;
    return 0;
}
