#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "config.h"
#include "utf8_args.h"
#include "paths.h"
#include "platform_compat.h"
#include "removed_transports.h"
#include "daemon/daemon.h"
#include "daemon/daemon_state.h"
#include "daemon/access_policy_arg.h"
#include "client/client_state.h"
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"
#include "logos_core.h"
#include "version_info.h"

static bool g_verbose = false;

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

// Resolve the --access-policy argument (see daemon/access_policy_arg.h for the
// three accepted spellings) and print the reason on stderr if it can't be.
static std::optional<std::string> resolveAccessPolicy(const std::string& arg)
{
    std::string error;
    auto resolved = logoscore::resolveAccessPolicyArg(arg, &error);
    if (!resolved)
        std::cerr << "Error: " << error << std::endl;
    return resolved;
}

int main(int argc, char *argv[])
{
    logosctl::Utf8Args utf8Args(argc, argv);

    // This binary is logoscore: the surface that exists today,
    // ~/.logoscore, JSON config. logosctl ships alongside it and
    // shares no state; this one changes only when it must.
    Config::setFlavor(Config::Flavor::Legacy);

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
    app.set_version_flag("--version", logosctl_version::versionString("logoscore"));
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

    // Shared option: modules directory (used by the daemon)
    std::vector<std::string> modulesDirs;
    auto* modulesDirOpt = app.add_option("-m,--modules-dir", modulesDirs,
        "Module search directory (repeatable)");

    std::string persistencePath;
    auto* persistencePathOpt = app.add_option("--persistence-path", persistencePath,
        "Base directory for module instance persistence (default: ~/.logoscore/data)");

    // --access-policy: inter-module access policy (the literal `enforce`, a
    // file path, or inline JSON). Daemon-only; forwarded to the runtime before
    // modules load. Absent => no policy => enforcement off, as before.
    std::string accessPolicyArg;
    auto* accessPolicyOpt = app.add_option("--access-policy", accessPolicyArg,
        "Inter-module access policy (default: none, no enforcement). "
        "`enforce` turns on deny-by-default: a module may only call the "
        "modules it declares as dependencies, and any other call is refused. "
        "Also accepts a path to a JSON file, or inline JSON "
        "(mode + per-target caller allowlists)");

    // Where modules run, and which directories count as bundled: the runtime's
    // placement policy, e.g. {"default":"inproc"}, and its bundled directories.
    std::string placementArg;
    auto* placementOpt = app.add_option("--placement", placementArg,
        "Where modules run, as the runtime's placement policy JSON, e.g. "
        "'{\"default\":\"inproc\"}' (default: the runtime's own)");
    std::vector<std::string> bundledDirsArg;
    auto* bundledDirsOpt = app.add_option("--bundled-modules-dir", bundledDirsArg,
        "A directory whose modules count as bundled: they may run in-process "
        "(repeatable)");

    // --access-group: share the daemon with an OS group. Sockets become
    // group-connectable (0660, chgrp'd) and the client artifacts group-readable,
    // so a second OS user in the group can drive the daemon (docker.sock model).
    std::string accessGroupArg;
    auto* accessGroupOpt = app.add_option("--access-group", accessGroupArg,
        "OS group to share the daemon with: makes the local sockets and client "
        "config/token group-accessible so a member can run logoscore commands");

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

    // --module-transport NAME=local is still accepted, as local is always bound;
    // the tcp and tcp_ssl listeners it also took are refused after parsing.
    std::vector<std::string> moduleTransportFlags;
    app.add_option("--module-transport", moduleTransportFlags)->group("");

    // Removed with the tcp and tcp_ssl transports: hidden, and refused if given.
    std::vector<const CLI::Option*> removedFlags;
    for (const char* flag : {"--insecure-tcp", "--no-verify-peer"})
        removedFlags.push_back(app.add_flag(flag)->group(""));
    for (const char* option : {"--client-transport", "--client-tcp-host", "--client-tcp-port",
                               "--client-codec", "--ssl-ca"})
        removedFlags.push_back(app.add_option(option)->group(""));

    // A token file in client/ for this client to present. Its env var serves
    // callers that drive logoscore as a subprocess; the flag wins over it.
    std::string clientTokenFile;
    auto* clientTokenFileOpt = app.add_option("--token-file", clientTokenFile,
        "Filename inside client/ to use for authentication (must already exist; "
        "no copy semantics)");
    clientTokenFileOpt->envname("LOGOSCORE_CLIENT_TOKEN_FILE");

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

    for (const CLI::Option* removed : removedFlags) {
        if (removed->count() == 0) continue;
        // get_name() is empty for a hidden option.
        std::cerr << "Error: " << removedWithTcpTransports("--" + removed->get_single_name())
                  << std::endl;
        return 1;
    }
    for (const std::string& spec : moduleTransportFlags) {
        const auto eq = spec.find('=');
        const std::string body = eq == std::string::npos ? std::string() : spec.substr(eq + 1);
        if (eq != 0 && body == "local") continue;
        const std::string protocol = body.substr(0, body.find(','));
        if (eq != 0 && (protocol == "tcp" || protocol == "tcp_ssl" || protocol == "local"))
            std::cerr << "Error: " << removedWithTcpTransports("--module-transport " + spec)
                      << std::endl;
        else
            std::cerr << "Error: --module-transport expects 'NAME=local', got: '" << spec
                      << "'" << std::endl;
        return 1;
    }

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
        logosctl::setEnvVar("LOGOSCORE_CONFIG_DIR", absCfgPath.string().c_str());
    }

    // ── Daemon mode ──────────────────────────────────────────────────────────
    if (daemonFlag || daemonSub->parsed()) {
        // Per-flag merge: load disk config (if any), then layer CLI
        // overrides on top — but only for flags the operator
        // explicitly passed. CLI11's Option::count() is the only
        // accurate signal: a default-valued local var is
        // indistinguishable from an explicit `--persistence-path ""`
        // without it. Anything not touched by either CLI or disk
        // falls through to defaults.
        DaemonConfig mergedCfg;
        std::string  configSource = "defaults";

        std::error_code cfgEc;
        if (auto disk = DaemonConfigFile::read()) {
            mergedCfg = *disk;
            configSource = "config.json";
        } else if (std::filesystem::exists(DaemonConfigFile::filePath(), cfgEc)) {
            // The reason is on stderr already. Defaults would drop what it says.
            std::cerr << "Error: the daemon does not start on a config it cannot load; fix "
                      << "or remove " << DaemonConfigFile::filePath() << "." << std::endl;
            return 1;
        }

        const bool anyCliFlag = (modulesDirOpt->count()      > 0)
                             || (persistencePathOpt->count() > 0)
                             || (accessPolicyOpt->count()    > 0)
                             || (placementOpt->count()       > 0)
                             || (bundledDirsOpt->count()     > 0)
                             || (accessGroupOpt->count()     > 0);
        if (anyCliFlag) configSource = "cli";

        if (modulesDirOpt->count() > 0)      mergedCfg.modulesDirs     = modulesDirs;
        // --persistence-path has to reach the field the daemon actually reads
        // — dirs.data — and not only its older spelling. The alias that folds
        // persistence_path into dirs.data lives inside daemonConfigFromJson,
        // so it runs while READING a config file and never after this merge:
        // assigning persistencePath alone left the flag with no consumer at
        // all. Writing both gives this block the precedence it documents
        // (explicit command line > config file > default) and keeps the two
        // spellings agreeing in whatever --persist-config writes back.
        //
        // Absolutised first, because the two sources spell relative paths
        // differently: a `dirs:` entry in config.json is relative to the config
        // dir (Config::setSessionDirOverride resolves it there), while a path
        // typed on the command line is relative to the shell's cwd — what this
        // flag has always meant, and what --modules-dir still means. Pinning it
        // here keeps `--persistence-path ./data` pointing at ./data instead of
        // quietly reparenting it under ~/.logoscore. An explicit empty value
        // stays empty: that is how both fields already encode "no override,
        // use the default", and absolute("") would turn it into the cwd.
        //
        // A leading `~/` is left ALONE so that resolveOverride expands it
        // (config.cpp:107). Absolutising it first would produce `$PWD/~/x` — a
        // real directory literally named `~` — and would make the flag and a
        // config.json `dirs.data` disagree about the same string. The shell
        // expands an unquoted `~/x` before we ever see it, so this only governs
        // a quoted or script-built value, which is exactly the case where the
        // user cannot have meant a directory called `~`.
        if (persistencePathOpt->count() > 0) {
            std::string resolved = persistencePath;
            const bool tilde = !resolved.empty() && resolved[0] == '~'
                            && (resolved.size() == 1 || resolved[1] == '/');
            if (!resolved.empty() && !tilde) {
                std::error_code ec;
                std::filesystem::path abs =
                    std::filesystem::absolute(resolved, ec);
                if (!ec) resolved = abs.lexically_normal().string();
            }
            mergedCfg.persistencePath = resolved;
            mergedCfg.dirs.data       = resolved;
        }
        if (accessGroupOpt->count() > 0)     mergedCfg.accessGroup     = accessGroupArg;
        if (placementOpt->count() > 0)       mergedCfg.placement       = placementArg;
        if (bundledDirsOpt->count() > 0) {
            mergedCfg.bundledModulesDirs.clear();
            for (const std::string& dir : bundledDirsArg) {
                std::error_code ec;
                const auto abs = std::filesystem::absolute(dir, ec);
                mergedCfg.bundledModulesDirs.push_back(ec ? dir : abs.lexically_normal().string());
            }
        }
        // Resolve --access-policy (file-or-inline); abort on bad input.
        if (accessPolicyOpt->count() > 0) {
            auto resolved = resolveAccessPolicy(accessPolicyArg);
            if (!resolved) return 1;
            mergedCfg.accessPolicy = std::move(*resolved);
        }
        return Daemon::start(argc, argv, mergedCfg, configSource, persistConfig, g_verbose);
    }

    // The removed client flags' environment variables are refused by client commands.
    if (!app.get_subcommands().empty()) {
        for (const char* env : {"LOGOSCORE_CLIENT_TRANSPORT", "LOGOSCORE_CLIENT_TCP_HOST",
                                "LOGOSCORE_CLIENT_TCP_PORT", "LOGOSCORE_CLIENT_NO_VERIFY_PEER",
                                "LOGOSCORE_CLIENT_CODEC", "LOGOSCORE_CLIENT_SSL_CA"}) {
            const char* value = std::getenv(env);
            if (!value || !*value) continue;
            std::cerr << "Error: " << removedWithTcpTransports(std::string("$") + env)
                      << std::endl;
            return 1;
        }
    }

    // ── Client-side --token-file merge ───────────────────────────────────────
    // defaults < client/config.json < --token-file. The merged result applies
    // to this run; with `--persist-config` it is also written back to
    // client/config.json so subsequent no-flag launches reproduce it.
    if (clientTokenFileOpt->count() > 0 || persistConfig) {
        ClientState merged = ClientStateFile::read();  // disk (or empty)
        // Both modules are dialed on their local sockets.
        merged.daemon.insert("core_service");
        merged.daemon.insert("capability_module");

        if (clientTokenFileOpt->count() > 0) {
            merged.tokenFile = clientTokenFile;
            // Refuse to start if the named raw-token file isn't
            // already under client/. No copy semantics — the
            // operator is expected to copy the daemon-side
            // tokens/<name>.json into place themselves.
            std::error_code ec;
            if (!std::filesystem::exists(
                    Config::clientTokenPath(clientTokenFile), ec)) {
                std::cerr << "Error: --token-file '" << clientTokenFile
                          << "' does not exist at "
                          << Config::clientDir() << "/" << clientTokenFile
                          << ". Copy it from the daemon's daemon/tokens/ dir first."
                          << std::endl;
                return 1;
            }
            // Existence isn't enough — validate the content now so a file
            // with no usable token errors here, not later at connect time.
            if (ClientStateFile::readTokenFile(clientTokenFile).empty()) {
                std::cerr << "Error: --token-file '" << clientTokenFile
                          << "' at " << Config::clientTokenPath(clientTokenFile)
                          << " has no usable 'token' field (missing key, "
                             "empty, or unparseable JSON)."
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

    // -m/-l/--persistence-path configure the daemon (-D) only. Inline (-c) mode
    // has been removed, so these flags are meaningless for a client subcommand
    // or a bare invocation — reject them with daemon/client guidance rather than
    // silently ignoring them.
    auto rejectDaemonOnlyFlags = [&]() -> bool {
        if (modulesDirOpt->count() == 0 && persistencePathOpt->count() == 0)
            return false;
        std::cerr <<
            "Error: -m/--modules-dir and --persistence-path apply only to the "
            "daemon (-D); inline (-c) mode has been removed. "
            "Use daemon + client commands:\n"
            "  logoscore -D -m <dir>                     # start a daemon (clean)\n"
            "  logoscore load-module <module>            # load a module\n"
            "  logoscore call <module> <method> [args]   # call a method\n";
        return true;
    };

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
    };

    for (auto& [sub, name] : clientSubs) {
        if (!sub->parsed())
            continue;

        if (rejectDaemonOnlyFlags())
            return 1;


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

        Output output(jsonMode);
        if (humanMode)
            output.setHumanMode(true);
        RpcClient rpcClient;

        auto cmd = createCommand(name, rpcClient, output);
        if (!cmd) {
            output.printError("INVALID_ARGS",
                              "Unknown command: " + name + ". Run 'logoscore --help' for usage.");
            return 1;
        }

        return cmd->execute(cmdArgs);
    }

    // ── Stray daemon-only flags without -D and without a subcommand ──────────
    if (rejectDaemonOnlyFlags())
        return 1;

    // ── No mode detected — show help ─────────────────────────────────────────
    std::cout << app.help() << std::endl;
    return 0;
}
