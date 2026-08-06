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
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "paths.h"
#include "platform_compat.h"
#include "daemon/daemon.h"
#include "daemon/daemon_state.h"
#include "daemon/log_sink.h"
#include "client/client_state.h"
#include "client/client.h"
#include "client/output.h"
#include "client/commands/command.h"
#include "logos_core.h"
#include "version_info.h"

static bool g_verbose = false;

// usleep(3) lives in mingw's <unistd.h>, which the Windows branch above does
// not include (it would drag in the CRT's POSIX-ish io layer next to
// windows.h). One named helper beats an #ifdef in the middle of the readiness
// poll.
static void sleepMs(unsigned ms)
{
#ifdef _WIN32
    ::Sleep(ms);
#else
    ::usleep(ms * 1000);
#endif
}

#ifdef _WIN32
namespace {

// Quote one argument the way CommandLineToArgvW (and therefore every Windows
// program's argv) parses it back. Windows has no argv vector at the API
// boundary -- CreateProcess takes a single string -- so the split has to be
// reconstructible or a --config-dir under "C:\Program Files\..." arrives at
// the daemon as two arguments.
//
// The rule that is easy to get wrong: a backslash is only an escape when it
// precedes a quote, so runs of backslashes are doubled *only* there. A blanket
// doubling would turn every Windows path in the command line into a
// double-separator mess.
std::string quoteArg(const std::string& a)
{
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos)
        return a;

    std::string out = "\"";
    for (std::size_t i = 0; ; ) {
        std::size_t backslashes = 0;
        while (i < a.size() && a[i] == '\\') { ++i; ++backslashes; }
        if (i == a.size()) {
            // Trailing backslashes precede the closing quote we are about to
            // add, so they do become escapes and must be doubled.
            out.append(backslashes * 2, '\\');
            break;
        }
        if (a[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(a[i]);
        }
        ++i;
    }
    out.push_back('"');
    return out;
}

// The Windows analogue of fork + setsid + reopen-stdio + execv.
//
// There is no fork here and none is wanted: the POSIX side only forks in order
// to immediately exec (see the comment at the call site -- macOS forbids
// running on after fork once CoreFoundation is up), so the whole sequence
// collapses into one CreateProcess.
//
//   DETACHED_PROCESS         is setsid(): the child gets no console at all, so
//                            it neither holds the terminal nor dies with it.
//   CREATE_NEW_PROCESS_GROUP keeps a Ctrl-C aimed at this console from
//                            reaching it.
//   STARTF_USESTDHANDLES     is the dup2 pair: stdin from NUL, stdout+stderr
//                            into the startup file, which is exactly what the
//                            POSIX branch does and for the same reason (early
//                            failures must be readable before LogSink exists).
//
// bInheritHandles must be TRUE or STARTF_USESTDHANDLES is ignored and the
// child silently inherits nothing usable.
bool spawnDetached(const std::string& exePath,
                   const std::vector<std::string>& args,
                   const std::string& startupPath,
                   PROCESS_INFORMATION& pi)
{
    std::string cmdline;
    for (const auto& a : args) {
        if (!cmdline.empty()) cmdline.push_back(' ');
        cmdline += quoteArg(a);
    }
    // CreateProcess may modify the command-line buffer in place, so it cannot
    // be a string literal or a c_str().
    std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back('\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hIn = ::CreateFileA("NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    HANDLE hOut = ::CreateFileA(startupPath.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE)
        hOut = ::CreateFileA("NUL", GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hIn;
    si.hStdOutput = hOut;
    si.hStdError  = hOut;

    const BOOL ok = ::CreateProcessA(exePath.c_str(), mutableCmd.data(),
                                     nullptr, nullptr, TRUE,
                                     DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                                     nullptr, nullptr, &si, &pi);
    const DWORD err = ok ? 0 : ::GetLastError();

    if (hIn  != INVALID_HANDLE_VALUE) ::CloseHandle(hIn);
    if (hOut != INVALID_HANDLE_VALUE) ::CloseHandle(hOut);

    if (!ok) {
        fprintf(stderr, "Error: could not start detached daemon '%s' "
                        "(CreateProcess failed, error %lu)\n",
                exePath.c_str(), static_cast<unsigned long>(err));
        return false;
    }
    return true;
}

}  // namespace
#endif  // _WIN32

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
        fprintf(stderr, "Critical: %s (%s:%d, %s)\n", localMsg.constData(), file, context.line, function);
        break;
    case QtFatalMsg:
        fprintf(stderr, "Fatal: %s (%s:%d, %s)\n", localMsg.constData(), file, context.line, function);
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
//     module load    ...  ->  load-module ...
//     token issue    ...  ->  issue-token ...
//     ... and so on for the rest of the module/token verbs.
//
// The hyphenated names on the right are internal dispatch tokens, hidden from
// --help; the groups are the surface. Doing the mapping in argv rather than
// with nested CLI11 subcommands avoids a collision with
// daemonSub->fallthrough(): a nested subcommand's unmatched arguments fall
// through to the parent and then to the top level, which rejects them
// ("The following argument was not expected: show").
static std::vector<std::string> normalizeGroupVerbs(int argc, char* argv[])
{
    // group -> verb -> internal subcommand
    static const std::map<std::string, std::map<std::string, std::string>> kGroups = {
        {"daemon", {
            {"config", "daemon-config"},
            {"start",  "daemon"},
            {"stop",   "stop"},
            {"status", "status"},
        }},
        {"client", {
            {"config", "client-config"},
        }},
        {"module", {
            {"ls",     "list-modules"},
            {"list",   "list-modules"},
            {"show",   "module-info"},
            {"info",   "module-info"},
            {"load",   "load-module"},
            {"unload", "unload-module"},
            {"reload", "reload-module"},
            {"stats",  "stats"},
        }},
        {"token", {
            {"issue",  "issue-token"},
            {"ls",     "list-tokens"},
            {"list",   "list-tokens"},
            {"revoke", "revoke-token"},
        }},
    };

    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        auto g = kGroups.find(a);
        // argv[0] is the program path; only look for a group from argv[1] on.
        if (i > 0 && g != kGroups.end() && i + 1 < argc) {
            auto v = g->second.find(argv[i + 1]);
            if (v != g->second.end()) {
                out.push_back(v->second);
                ++i;
                continue;
            }
        }
        out.push_back(a);
    }
    return out;
}

int main(int argc, char *argv[])
{
    // This binary is logosctl: new surface, own session directory,
    // YAML config. Set before any Config::* call.
    Config::setFlavor(Config::Flavor::Modern);

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
    CLI::App app{"logosctl - Logos Core runtime CLI"};
    app.set_version_flag("--version", logosctl_version::versionString("logosctl"));
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

    // Detach after the daemon is actually up. Backgrounding with `&` returns
    // immediately, before the transports bind, so the very next client command
    // races the boot and fails with "no daemon". --detach waits for the state
    // file to appear and only then lets the parent exit, which makes
    // `daemon start --detach && module ls` reliable in scripts and doctests.
    bool detach = false;
    app.add_flag("-d,--detach", detach,
                 "Fork into the background and return once the daemon is accepting commands");

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
        "Session directory to use (default: ~/.logosctl; also LOGOSCTL_CONFIG_DIR). "
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
    // Declared so `--help` lists them; their verbs are collapsed in argv by
    // normalizeGroupVerbs before CLI11 ever sees them, so reaching these
    // means the user typed a group with a missing or unknown verb.
    auto* moduleGroupSub   = app.add_subcommand("module",
        "Runtime modules: ls | show NAME | load NAME | unload NAME | reload NAME | stats");
    auto* tokenGroupSub    = app.add_subcommand("token",
        "Client tokens: issue --name N | ls | revoke NAME");
    auto* clientGroupSub   = app.add_subcommand("client",
        "Client-side configuration: config show | config set FILE");
    moduleGroupSub->allow_extras();
    tokenGroupSub->allow_extras();
    clientGroupSub->allow_extras();
    // Top-level shortcuts for the two package verbs that have no
    // runtime-module meaning, so they cannot be confused with `module ...`.
    auto* installSub       = app.add_subcommand("install",
        "Alias for 'package install'");
    auto* searchSub        = app.add_subcommand("search",
        "Alias for 'package search'");

    // The hyphenated names are internal dispatch tokens, not surface: the
    // groups above are what users type and what --help lists. Hiding them
    // keeps a ~20-entry flat list from competing with the grouped one.
    for (auto* sub : {loadModuleSub, unloadModuleSub, reloadModuleSub,
                      listModulesSub, moduleInfoSub, infoSub,
                      issueTokenSub, revokeTokenSub, listTokensSub,
                      daemonConfigSub, clientConfigSub, stopSub}) {
        sub->group("");
    }

    // Allow extras on all client subcommands so their positional args and
    // command-specific flags pass through to the Command objects unchanged
    for (auto* sub : {statusSub, loadModuleSub, unloadModuleSub, reloadModuleSub,
                      listModulesSub, moduleInfoSub, infoSub, callSub,
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
        logosctl::setEnvVar("LOGOSCTL_CONFIG_DIR", absCfgPath.string().c_str());
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
            qapp.setApplicationName("logosctl");
            qapp.setApplicationVersion(QString::fromStdString(logosctl_version::version()));
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

        // Reaching a bare group means normalizeGroupVerbs found no verb to
        // collapse, i.e. the verb was missing or misspelled.
        if (moduleGroupSub->parsed()) {
            fprintf(stderr, "Error: usage: logosctl module "
                            "<ls|show|load|unload|reload|stats> [name]\n");
            return 1;
        }
        if (tokenGroupSub->parsed()) {
            fprintf(stderr, "Error: usage: logosctl token "
                            "<issue --name N|ls|revoke NAME>\n");
            return 1;
        }
        if (clientGroupSub->parsed()) {
            fprintf(stderr, "Error: usage: logosctl client config "
                            "<show|set FILE>\n");
            return 1;
        }
    }

    // ── Daemon mode ──────────────────────────────────────────────────────────
    if (daemonFlag || daemonSub->parsed()) {
        if (detach) {
            // Re-exec rather than simply carrying on in the forked child.
            // macOS refuses to let a process that has already initialised
            // CoreFoundation (which the Qt/liblogos link pulls in before main)
            // keep running after fork() -- it aborts with
            // "you MUST exec()". So the child execs a fresh copy of this
            // binary with --detach stripped, and that copy runs the daemon
            // normally.
            const std::string self = paths::relaunchPath(argv[0]);
            if (self.empty()) {
                fprintf(stderr, "Error: could not resolve own path for --detach.\n");
                return 1;
            }

            std::vector<std::string> childArgs;
            childArgs.push_back(self);
            for (int i = 1; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "-d" || a == "--detach") continue;
                childArgs.push_back(a);
            }
            // The session must be explicit in the child: it no longer shares
            // this process's environment-derived default if the caller used
            // --config-dir, and an inherited-but-different session would be a
            // very confusing bug.
            logosctl::setEnvVar("LOGOSCTL_CONFIG_DIR", Config::configDir().c_str());

            const std::string statePath = Config::daemonStatePath();
            // A stale state file from a previous run would make the readiness
            // check pass instantly against a daemon that never started.
            {
                std::error_code ec;
                std::filesystem::remove(statePath, ec);
                std::filesystem::create_directories(Config::daemonDir(), ec);
            }

            // Somewhere for the child's *pre-logging* output to land.
            //
            // A config that fails validation -- a bad transport, a plaintext
            // listener on a public interface -- is rejected before LogSink
            // opens the real log, so with the child's stderr on /dev/null the
            // reason vanished and this command reported only "exited during
            // startup. See <log>" naming a file that was never created. Once
            // LogSink starts it dup2s its own pipe over these descriptors, so
            // this file only ever holds the early output, and it is removed
            // either way.
            const std::string startupPath =
                (std::filesystem::path(Config::daemonDir()) / "startup.err").string();
            { std::error_code ec; std::filesystem::remove(startupPath, ec); }

#ifdef _WIN32
            // One CreateProcess replaces fork + setsid + the stdio reopen +
            // execv; see spawnDetached above.
            PROCESS_INFORMATION childProc{};
            if (!spawnDetached(self, childArgs, startupPath, childProc))
                return 1;
            const long childPid = static_cast<long>(childProc.dwProcessId);
            // Has the child exited yet? A process handle is signalled exactly
            // when the process has, which is the WNOHANG waitpid equivalent
            // without GetExitCodeProcess's ambiguous STILL_ACTIVE (259).
            auto childExited = [&childProc]() {
                return ::WaitForSingleObject(childProc.hProcess, 0) == WAIT_OBJECT_0;
            };
            auto releaseChild = [&childProc]() {
                ::CloseHandle(childProc.hThread);
                ::CloseHandle(childProc.hProcess);
            };
#else
            const pid_t child = fork();
            if (child < 0) {
                perror("Error: could not fork for --detach");
                return 1;
            }
            if (child == 0) {
                // Between fork and exec only async-signal-safe calls are used.
                setsid();
                // Detach stdio from the inherited terminal. stdout/stderr go to
                // the startup file rather than /dev/null so a failure that
                // happens before LogSink opens the real log is still readable;
                // LogSink takes these descriptors over as soon as it starts.
                int devnull = ::open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    ::dup2(devnull, STDIN_FILENO);
                    if (devnull > STDERR_FILENO) ::close(devnull);
                }
                int early = ::open(startupPath.c_str(),
                                   O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (early < 0) early = ::open("/dev/null", O_WRONLY);
                if (early >= 0) {
                    ::dup2(early, STDOUT_FILENO);
                    ::dup2(early, STDERR_FILENO);
                    if (early > STDERR_FILENO) ::close(early);
                }
                std::vector<char*> cargv;
                for (auto& a : childArgs) cargv.push_back(const_cast<char*>(a.c_str()));
                cargv.push_back(nullptr);
                execv(self.c_str(), cargv.data());
                _exit(127);  // exec failed
            }
            const long childPid = static_cast<long>(child);
            auto childExited = [child]() {
                int status = 0;
                return ::waitpid(child, &status, WNOHANG) == child;
            };
            auto releaseChild = []() {};
#endif

            // Parent: wait for the child to publish state.json, which it
            // writes only after every transport has bound. Returning before
            // that would hand the caller a daemon that is not listening yet,
            // and the very next command would race it.
            // Resolve where the child will log by reading the same config it
            // will read. The real file carries a start-time stamp only the
            // child knows, so report the stable symlink beside it -- always
            // current, and printable before the child has booted.
            std::string logPath;
            {
                LoggingConfig lg;
                SessionDirs   sd;
                if (auto disk = DaemonConfigFile::read()) {
                    lg = disk->logging;
                    sd = disk->dirs;
                }
                if (!lg.enabled) {
                    logPath = "(file logging disabled)";
                } else {
                    Config::setSessionDirOverride(Config::SessionDir::Logs, sd.logs);
                    logPath = LogSink::stablePath(Config::logsDir(), lg.file);
                    // Leave no override behind: the child re-applies it from
                    // config, and the parent is about to exit anyway.
                    Config::setSessionDirOverride(Config::SessionDir::Logs, "");
                }
            }
            // Whatever the child managed to say before logging was up. Usually
            // empty; when it is not, it is the actual reason startup failed.
            auto earlyOutput = [&]() -> std::string {
                std::ifstream in(startupPath);
                if (!in) return {};
                std::string s((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                return s;
            };
            auto cleanupStartupFile = [&]() {
                std::error_code ec;
                std::filesystem::remove(startupPath, ec);
            };

            // The tail of the daemon's own log.
            //
            // The startup file only ever holds output from BEFORE LogSink
            // starts, because LogSink dup2s its pipe over those descriptors.
            // So a daemon that dies *after* logging is up leaves the startup
            // file empty and the reason in the log -- and pointing at a path
            // is no help to a CI run, a script, or anyone who then has to go
            // and read it. Print the end of it.
            auto logTail = [&](std::size_t lines) -> std::string {
                if (logPath.empty()) return {};
                std::ifstream in(logPath);
                if (!in) return {};
                std::vector<std::string> tail;
                std::string line;
                while (std::getline(in, line)) {
                    tail.push_back(line);
                    if (tail.size() > lines) tail.erase(tail.begin());
                }
                std::string out;
                for (const auto& l : tail) { out += l; out += '\n'; }
                return out;
            };

            for (int i = 0; i < 600; ++i) {
                std::error_code ec;
                if (std::filesystem::exists(statePath, ec)) {
                    cleanupStartupFile();
                    fprintf(stdout, "Daemon started (pid %ld)\nLogs: %s\n",
                            childPid, logPath.c_str());
                    releaseChild();
                    return 0;
                }
                if (childExited()) {
                    const std::string early = earlyOutput();
                    if (!early.empty()) {
                        // Print the child's own message. Prefixing it with
                        // "Error: daemon exited" and a log path would bury the
                        // one line that says what to fix.
                        fprintf(stderr, "%s\n", early.c_str());
                    } else {
                        // Nothing before LogSink started, so the reason is in
                        // the log. Show it rather than naming it.
                        const std::string tail = logTail(30);
                        fprintf(stderr, "Error: daemon exited during startup.\n");
                        if (!tail.empty())
                            fprintf(stderr, "--- %s (last lines) ---\n%s",
                                    logPath.c_str(), tail.c_str());
                        else
                            fprintf(stderr, "See %s\n", logPath.c_str());
                    }
                    cleanupStartupFile();
                    releaseChild();
                    return 1;
                }
                sleepMs(100);
            }
            releaseChild();
            {
                const std::string early = earlyOutput();
                fprintf(stderr, "Error: daemon did not become ready in 60s.\n");
                if (!early.empty()) fprintf(stderr, "%s\n", early.c_str());
                const std::string tail = logTail(30);
                if (!tail.empty())
                    fprintf(stderr, "--- %s (last lines) ---\n%s",
                            logPath.c_str(), tail.c_str());
                else
                    fprintf(stderr, "See %s\n", logPath.c_str());
            }
            cleanupStartupFile();
            return 1;
        }

        QCoreApplication qapp(argc, argv);
        qapp.setApplicationName("logosctl");
        qapp.setApplicationVersion(QString::fromStdString(logosctl_version::version()));

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

        // Push the top-level `ssl:` block into the listeners that need it.
        // It is a session-wide default: a `tcp_ssl` entry naming its own
        // cert/key/ca keeps them, one that names none inherits these. Done
        // before state.json is written, so the resolved snapshot shows the
        // material each listener actually bound with rather than the intent
        // it was derived from.
        //
        // logosctl-only by construction -- this is main.cpp, the Modern
        // front-end. logoscore (main_legacy.cpp) never calls it.
        applySslDefaults(mergedCfg);

        // Make sure the well-known modules at least *have* an entry,
        // so a bare `logosctl daemon start` (no transport flags) still boots
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

        // Plaintext-TCP guard, post-merge: refuse to bind plaintext tcp on a
        // non-loopback host unless `insecure_tcp` is enabled. Iterating the
        // merged map means a disk-supplied plaintext listener gets the same
        // scrutiny as any other — the operator can't bypass the guard by
        // stashing the combo in the config file.
        for (const auto& [moduleName, transports] : mergedCfg.modules) {
            for (const auto& t : transports) {
                if (t.protocol != "tcp") continue;
                if (isLoopback(t.host)) continue;
                if (mergedCfg.insecureTcp) continue;
                // Name the escape hatch this binary actually has. logosctl has
                // no --insecure-tcp flag -- configuration is the YAML document
                // -- and telling an operator to pass a flag that does not exist
                // sends them looking for a typo in their own command line.
                std::cerr << "Error: module '" << moduleName
                          << "' binds plaintext tcp on non-loopback host '"
                          << t.host << "'. Use protocol: tcp_ssl, or set "
                          << "insecure_tcp: true in the daemon config if you "
                          << "really mean it."
                          << std::endl;
                return 1;
            }
        }

        // TLS-material guard, post-defaults: a tcp_ssl listener with no
        // certificate binds happily and then fails every handshake with
        // "no shared cipher", which reads like a client problem. Refuse to
        // start and name the two places the material can come from.
        if (auto missing = findTlsListenersMissingMaterial(mergedCfg);
            !missing.empty()) {
            for (const auto& who : missing) {
                std::cerr << "Error: " << who << " has no certificate/key. "
                          << "Set cert: and key: on the listener, or a "
                          << "top-level ssl: { cert, key } block to cover "
                          << "every tcp_ssl listener at once." << std::endl;
            }
            return 1;
        }

        // Both trailing parameters are bool. Name them at the call site: this
        // line used to read `..., configSource, g_verbose)`, which silently
        // handed g_verbose to persistConfig and left verbose false forever --
        // so -v never reached anything the daemon gates on it, and it quietly
        // rewrote the config file instead. logosctl has no --persist-config;
        // configuration is installed with `daemon config set`.
        return Daemon::start(argc, argv, mergedCfg, configSource,
                             /*persistConfig=*/false, /*verbose=*/g_verbose);
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
        qapp.setApplicationName("logosctl");
        qapp.setApplicationVersion(QString::fromStdString(logosctl_version::version()));

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
        // pre-supplied: `logosctl install X` is `logosctl package install X`.
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
                              "Unknown command: " + name + ". Run 'logosctl --help' for usage.");
            return 1;
        }

        return cmd->execute(cmdArgs);
    }

    // ── No mode detected — show help ─────────────────────────────────────────
    std::cout << app.help() << std::endl;
    return 0;
}
