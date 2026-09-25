#include "daemon.h"

#include <spdlog/spdlog.h>
#include "daemon_state.h"
#include "port_allocator.h"
#include "token_store.h"
#include "log_sink.h"
#include "package_bootstrap.h"
#include "../config.h"
#include "../paths.h"
#include "logos_core.h"

#include <logos_socket_paths.h>
#include <logos_transport_config.h>
#include <logos_transport_config_json.h>
#include <logos_protocol.h>
#include "../core_service/package_service.h"
#include "../core_service/shell_calls.h"
#include "../plain_rpc.h"
#include "../local_endpoint.h"

#include <uuid.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "../platform_compat.h"
#include "../process_util.h"

#ifdef _WIN32
#include <process.h>   // getpid — mingw-w64 puts it here, not in unistd.h
#include <windows.h>
#else
#include <unistd.h>
#endif

static volatile sig_atomic_t g_shutdownRequested = 0;
static std::mutex g_shutdownMutex;
static std::condition_variable g_shutdownChanged;

void Daemon::signalHandler(int signal)
{
    (void)signal;
    g_shutdownRequested = 1;
}

#ifdef _WIN32
namespace {
// Ctrl-C, Ctrl-Break and the console-window/logoff/shutdown events all arrive
// here. Returning TRUE means "handled", which for CTRL_C_EVENT stops the
// default terminate-the-process behaviour and lets the shutdown below unwind
// normally (unlinking state.json and closing the local pipes).
//
// For CTRL_CLOSE/LOGOFF/SHUTDOWN Windows grants a bounded grace period and
// then kills the process regardless — that is the OS contract, not something
// this handler can extend.
BOOL WINAPI consoleCtrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        Daemon::requestShutdown();
        return TRUE;
    default:
        return FALSE;
    }
}
}  // namespace

#endif

void Daemon::requestShutdown()
{
    g_shutdownRequested = 1;
    g_shutdownChanged.notify_all();
}

void Daemon::requestShutdownAfter(int delayMs)
{
    std::thread([delayMs] {
        if (delayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        Daemon::requestShutdown();
    }).detach();
}

void Daemon::setupSignalHandlers()
{
#ifdef _WIN32
    // Ctrl-C reaches a console process through this, not through signal():
    // the CRT's SIGINT emulation is itself implemented on top of it, and only
    // this form sees CTRL_CLOSE_EVENT (the window's X button).
    ::SetConsoleCtrlHandler(&consoleCtrlHandler, TRUE);

    // Windows has no way for one process to send SIGTERM to another, so a
    // raise() from inside this process is the only thing that can arrive here.
    // Registered anyway so that path shuts down the same way rather than
    // taking the CRT default of terminating outright.
    std::signal(SIGINT, &Daemon::signalHandler);
    std::signal(SIGTERM, &Daemon::signalHandler);
#else
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

namespace {

// Materialize a per-module TransportInfo list into a LogosTransportSet
// used by the runtime's child host. For non-LocalSocket entries with
// `port == 0`, pre-allocate a fresh ephemeral port via PortAllocator
// (ask the kernel for a free TCP port, close the probe socket, hand
// the number to the listener). Without the pre-allocation the listener
// would race the kernel to bind and the actual port would only be
// known *after* the listener was up — too late to advertise in
// state.json.
//
// No "inheritance" here. Each module's transports are independent;
// nothing about core_service's set leaks into capability_module's.
// The CLI's `--module-transport` flags drive the input directly.
//
// Returns std::nullopt if any non-local listener fails to acquire a
// port. The caller is expected to abort daemon startup — silently
// advertising port=0 in state.json (the previous behaviour) is
// worse: clients pick up an unreachable endpoint and time out.
std::optional<LogosTransportSet> buildTransportSet(
    const std::vector<TransportInfo>& infos,
    const std::string& moduleName)
{
    LogosTransportSet out;
    for (const auto& src : infos) {
        TransportInfo eff = src;

        if (src.protocol != "local" && eff.port == 0) {
            eff.port = PortAllocator::allocateEphemeralTcp(eff.host);
            if (eff.port == 0) {
                fprintf(stderr,
                        "[%s] Failed to allocate ephemeral port for %s\n",
                        moduleName.c_str(), src.protocol.c_str());
                return std::nullopt;
            }
        }

        LogosTransportConfig c;
        if (eff.protocol == "local") c.protocol = LogosProtocol::QtRemotePlain;
        else if (eff.protocol == "tcp") c.protocol = LogosProtocol::Tcp;
        else if (eff.protocol == "tcp_ssl") c.protocol = LogosProtocol::TcpSsl;
        else {
            fprintf(stderr,
                    "[%s] Unknown transport '%s'.\n",
                    moduleName.c_str(), eff.protocol.c_str());
            return std::nullopt;
        }
        c.host       = eff.host;
        c.port       = eff.port;
        c.caFile     = eff.caFile;
        c.certFile   = eff.certFile;
        c.keyFile    = eff.keyFile;
        c.verifyPeer = eff.verifyPeer;
        c.codec      = (eff.codec == "cbor") ? LogosWireCodec::Cbor
                                              : LogosWireCodec::Json;
        out.push_back(std::move(c));
    }
    return out;
}

// Round-trip a LogosTransportSet back into the on-disk TransportInfo
// shape so we can advertise it under `modules.<name>.transports` in
// state.json. Reverse of buildTransportSet in the sense that the
// on-disk shape matches what clients then read.
std::vector<TransportInfo> toAdvertised(const LogosTransportSet& set)
{
    std::vector<TransportInfo> out;
    for (const auto& c : set) {
        TransportInfo t;
        switch (c.protocol) {
        case LogosProtocol::Tcp:         t.protocol = "tcp"; break;
        case LogosProtocol::TcpSsl:      t.protocol = "tcp_ssl"; break;
        case LogosProtocol::QtRemotePlain: t.protocol = "local"; break;
        case LogosProtocol::LocalSocket:
        default:                         t.protocol = "local"; break;
        }
        t.host = c.host;
        t.port = c.port;
        t.caFile = c.caFile;
        t.verifyPeer = c.verifyPeer;
        t.codec = (c.codec == LogosWireCodec::Cbor) ? "cbor" : "json";
        // certFile/keyFile intentionally NOT copied — they're
        // server-only secrets and don't belong in state.json.
        out.push_back(std::move(t));
    }
    return out;
}

// Load the bundled package modules and point package_manager at this
// session's directories. Best-effort throughout: a daemon that cannot manage
// packages is still fully usable for loading and calling modules, so nothing
// here is allowed to abort startup.
//
// The sequencing — which failure skips what — lives in package_bootstrap so it
// can be tested without a running core. This function is only the wiring from
// those hooks to core_service, over the daemon's shell binding, and the live
// plain provider.
void bootstrapPackageModules(logosctl::PlainRpcContext* api,
                             const std::string& bundledDir,
                             const std::string& signaturePolicy,
                             bool verbose)
{
    package_bootstrap::Hooks hooks;

    hooks.loadModule = [](const std::string& name) { return shell_calls::load(name); };
    hooks.unloadModule = [](const std::string& name) {
        shell_calls::unload(name, /*withDependents=*/true);
    };
    hooks.warn = [](const std::string& line) {
        fprintf(stderr, "%s\n", line.c_str());
    };
    if (verbose)
        hooks.note = [](const std::string& line) {
            fprintf(stderr, "%s\n", line.c_str());
        };

    // The CallError overload rather than the json one: a setter returns void,
    // so a dispatched call and a call that never reached the module are
    // indistinguishable in the return value. `err` is what tells them apart
    // ("object_unavailable" when the target object cannot be acquired), and
    // that distinction is what the signature-policy fail-closed rests on.
    hooks.configure = [api](const std::string& method,
                            const std::vector<std::string>& args) {
        logosctl::PlainRpcClient* pm =
            api ? api->client(package_bootstrap::kPackageManager) : nullptr;
        if (!pm) return false;
        logosctl::PlainRpcError err;
        pm->invoke(method, args, 0, &err);
        return err.ok();
    };

    package_bootstrap::Dirs dirs;
    // bundledDir is <bin>/../modules; its plugins sibling is alongside it.
    if (!bundledDir.empty()) {
        dirs.embeddedModules   = bundledDir;
        dirs.embeddedUiPlugins =
            (std::filesystem::path(bundledDir).parent_path() / "plugins").string();
    }
    dirs.userModules   = Config::modulesDir();
    dirs.userUiPlugins = Config::pluginsDir();
    dirs.keyring       = Config::keyringDir();

    package_bootstrap::run(hooks, dirs, signaturePolicy);
}

char* copyForProtocol(const std::string& value)
{
    auto* result = static_cast<char*>(std::malloc(value.size() + 1));
    if (!result) return nullptr;
    std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}

// core_service runs in liblogos; the daemon names its operators, handles its
// shutdown and adds the package operations.
char* resolveOperator(const char* token, const char* transport, void*)
{
    if (!token || !transport) return nullptr;
    // A caller in this process is as local as the local socket.
    const std::string protocol = std::string(transport) == "inproc" ? "local" : transport;
    TokenStore tokenStore;
    const auto name = tokenStore.lookupByToken(token, protocol);
    return name ? copyForProtocol(*name) : nullptr;
}

// Milliseconds between answering `shutdown` and leaving the wait loop, so the
// reply and anything else in flight get a turn. $LOGOSCTL_SHUTDOWN_GRACE_MS
// overrides it; the daemon-stop integration test pins it to 0.
int shutdownGraceMs()
{
    static const int ms = []() {
        constexpr int kDefault = 200;
        const char* v = std::getenv("LOGOSCTL_SHUTDOWN_GRACE_MS");
        if (!v || !*v) return kDefault;
        char* end = nullptr;
        const long n = std::strtol(v, &end, 10);
        if (end == v || *end != '\0' || n < 0 || n > 60000) return kDefault;
        return static_cast<int>(n);
    }();
    return ms;
}

void requestShutdownFromCoreService(void*)
{
    Daemon::requestShutdownAfter(shutdownGraceMs());
}

// Package operations change what is installed: the runtime and operators only.
char* extendCoreService(const char* callerJson, const char* method, const char* argsJson,
                        void* userData)
{
    const std::string name = method ? method : "";
    if (!PackageService::owns(name)) return nullptr;
    const nlohmann::json caller = nlohmann::json::parse(callerJson ? callerJson : "{}",
                                                        nullptr, false);
    const std::string kind = caller.is_object() ? caller.value("kind", std::string{}) : "";
    if (kind != "host" && kind != "operator")
        return copyForProtocol(nlohmann::json{
            {"status", "error"}, {"code", "FORBIDDEN"},
            {"message", "core_service." + name + " is for operators."}}.dump());
    const nlohmann::json args = nlohmann::json::parse(
        argsJson && *argsJson ? argsJson : "[]", nullptr, false);
    std::lock_guard<std::mutex> turn(shell_calls::packageOperations());
    auto result = args.is_array() ? static_cast<PackageService*>(userData)->call(name, args)
                                  : std::nullopt;
    if (!result)
        return copyForProtocol(nlohmann::json{
            {"status", "error"}, {"code", "INVALID_ARGS"},
            {"message", "core_service." + name + " takes other arguments."}}.dump());
    return copyForProtocol(result->dump());
}

// What the embedded core_service adds to its own inproc and local listeners.
LogosTransportSet networkTransports(const LogosTransportSet& all)
{
    LogosTransportSet network;
    for (const auto& transport : all)
        if (transport.protocol == LogosProtocol::Tcp || transport.protocol == LogosProtocol::TcpSsl)
            network.push_back(transport);
    return network;
}

} // namespace

int Daemon::start(int argc, char* argv[],
                  const DaemonConfig& cfg,
                  const std::string& configSource,
                  bool persistConfig,
                  bool verbose)
{
    g_shutdownRequested = 0;
    const auto& modulesDirs      = cfg.modulesDirs;

    // Apply the session-directory redirects before anything asks Config for a
    // path. Defaults keep every directory inside the config dir, which is what
    // makes a session portable; an override is an explicit decision to move
    // one out (a shared keyring, a cache on a bigger disk, a modules tree
    // something else manages).
    // Everything gated on this is a logosctl feature; logoscore keeps the
    // behaviour it has today.
    const bool modern = (Config::flavor() == Config::Flavor::Modern);

    Config::setSessionDirOverride(Config::SessionDir::Modules, cfg.dirs.modules);
    Config::setSessionDirOverride(Config::SessionDir::Plugins, cfg.dirs.plugins);
    Config::setSessionDirOverride(Config::SessionDir::Keyring, cfg.dirs.keyring);
    Config::setSessionDirOverride(Config::SessionDir::Data,    cfg.dirs.data);
    Config::setSessionDirOverride(Config::SessionDir::Cache,   cfg.dirs.cache);
    Config::setSessionDirOverride(Config::SessionDir::Logs,    cfg.dirs.logs);

    // Start capturing before anything else runs, so the log holds the whole
    // boot -- including a failure during it, which is exactly when the log is
    // worth having. Non-fatal: a daemon that cannot write a log file is still
    // a working daemon.
    if (modern) {
        LogSink::Options lo;
        lo.enabled   = cfg.logging.enabled;
        lo.dir       = Config::logsDir();
        lo.file      = cfg.logging.file;
        lo.maxSizeMb = cfg.logging.maxSizeMb;
        lo.maxFiles  = cfg.logging.maxFiles;
        // Mirror whenever configured, pipe or terminal alike: a caller doing
        // `daemon start > out.log` is watching that pipe. Detached, the
        // original stdout is /dev/null, so this costs a discarded write.
        lo.console   = cfg.logging.console;
        if (cfg.logging.enabled && !LogSink::instance().start(lo)) {
            fprintf(stderr, "Warning: could not open the log file under %s; "
                            "continuing without file logging.\n",
                    lo.dir.c_str());
        }
    }
    const auto& moduleTransports = cfg.modules;
    // 1. Generate instance ID BEFORE core init, so logos_host inherits it
    std::random_device rd;
    std::mt19937 gen(rd());
    uuids::uuid_random_generator uuidGen(gen);

    // instanceId: 12 hex chars (no dashes), like QUuid::Id128.left(12)
    std::string fullUuid = uuids::to_string(uuidGen());
    std::string instanceId;
    for (char c : fullUuid)
        if (c != '-') instanceId += c;
    instanceId = instanceId.substr(0, 12);

    int64_t pid = getpid();

    logosctl::setEnvVar("LOGOS_INSTANCE_ID", instanceId.c_str());

    // Share the node with an OS group if the operator asked (--access-group).
    // Validate the group ONCE here, up front, so the socket policy and the
    // client-artifact policy agree: a typo'd group name is rejected in both
    // places (rather than exporting env vars for a group that
    // writeLocalClientArtifacts would then decline). When known-good, export
    // LOGOS_SOCKET_GROUP + LOGOS_SOCKET_MODE=0660 BEFORE logos_core_init so
    // every module subprocess (logos_host) and its children inherit it and
    // apply the policy to the local socket they bind (see logos-protocol's
    // applySocketPerms) — 0660 grants the write permission an AF_UNIX connect()
    // requires. `effectiveAccessGroup` (empty when unset or invalid) is what
    // gets handed to writeLocalClientArtifacts below.
    std::string effectiveAccessGroup = cfg.accessGroup;
#ifdef _WIN32
    // Refused outright rather than degraded to owner-only. Every mechanism the
    // feature is built from is POSIX: an OS group database (getgrnam_r), a gid
    // on the socket (chown), and a 0660 mode that an AF_UNIX connect() checks.
    // Windows has none of them — QLocalServer is a named pipe, whose access is
    // a security descriptor set at creation, and there is no group to name.
    // Accepting the flag and quietly not sharing would tell an operator their
    // node is group-restricted when it is in fact only user-restricted.
    if (!effectiveAccessGroup.empty()) {
        fprintf(stderr,
                "Error: --access-group is not supported on Windows. Sharing a "
                "node with an OS group needs POSIX group ownership and socket "
                "mode bits, which named pipes do not have.\n");
        return 1;
    }
#else
    if (!effectiveAccessGroup.empty()) {
        gid_t gid = 0;
        if (!resolveOsGroupGid(effectiveAccessGroup, gid)) {
            fprintf(stderr,
                    "Warning: --access-group '%s' is not a known group; the "
                    "daemon will not be shared.\n",
                    effectiveAccessGroup.c_str());
            effectiveAccessGroup.clear();
        } else {
            logosctl::setEnvVar("LOGOS_SOCKET_GROUP", effectiveAccessGroup.c_str());
            logosctl::setEnvVar("LOGOS_SOCKET_MODE", "0660");
        }
    }
#endif

    // Refuse to start if a live daemon already owns this config-dir — two would
    // clobber the shared state.json and re-issue the auto-token. Checked before
    // logos_core_init so it fails fast. A stale file from a crashed daemon (pid
    // gone) is not a live owner and is overwritten below.
    {
        const DaemonRuntimeState existing = DaemonRuntimeStateFile::read();
        if (existing.fileOk && existing.pid > 0
            && logosctl::processAlive(existing.pid)) {
            fprintf(stderr,
                    "Error: a logosctl daemon is already running in this config dir "
                    "(pid %lld, instance %s). Refusing to start a second one — use "
                    "--config-dir for a parallel instance.\n",
                    static_cast<long long>(existing.pid),
                    existing.instanceId.c_str());
            return 1;
        }
    }

    // Reap socket files left behind by a previous node that died without
    // running its destructors. A graceful stop unlinks its own sockets during
    // provider teardown, so this is purely for
    // the paths that cannot unwind: SIGKILL, a module crash, and the
    // PR_SET_PDEATHSIG kill that reaps orphaned logos_host children. Without
    // it those files accumulate in the temp dir forever, one per module per
    // boot.
    //
    // Deliberately placed AFTER the already-running check and BEFORE
    // logos_core_init: at this point no socket of ours exists yet, so the
    // reaper cannot race its own endpoints. A *co-resident* node's sockets are
    // live, and logos::isSocketDead fails closed — it unlinks only an S_ISSOCK
    // inode that we own and whose connect() is refused — so a second daemon
    // sharing the temp dir keeps its sockets, and a regular file that merely
    // shares the prefix (e.g. a logos_*.lgx build artefact) is never touched.
    //
    // Use the same Qt-compatible directory as local endpoint discovery,
    // including macOS's per-user fallback when TMPDIR is absent.
    {
        const std::string tempPath = logosctl::localTransportTempDirectory();
        const std::size_t reaped = logos::reapStaleSockets(tempPath, "logos_");
        if (reaped > 0 && verbose)
            fprintf(stderr, "Reaped %zu stale socket file(s) from %s\n",
                    reaped, tempPath.c_str());
    }

    // 2. Initialize logos core
    logos_core_init(argc, argv);

    // 3. Add plugin directories — user-specified and bundled
    //    Resolve to absolute paths: logos_core cannot load plugin metadata from relative paths.
    for (const std::string& dir : modulesDirs) {
        std::error_code ec;
        std::string absDir = std::filesystem::absolute(dir, ec).string();
        const char* resolved = ec ? dir.c_str() : absDir.c_str();
        if (verbose)
            fprintf(stderr, "Added plugins directory: %s\n", resolved);
        logos_core_add_modules_dir(resolved);
    }

    std::string bundledDir = paths::bundledModulesDir();
    if (!bundledDir.empty()) {
        logos_core_add_modules_dir(bundledDir.c_str());
        if (verbose)
            fprintf(stderr, "Added bundled modules directory: %s\n", bundledDir.c_str());
    }

    // 3b. The session's own writable modules directory — where anything
    //     installed into this session lands. Without it on the search path,
    //     `install` would put a module on disk that the daemon could never
    //     see, so install-then-load could not work at all. Created eagerly so
    //     the package manager has somewhere to write on its very first
    //     install rather than failing on a missing directory.
    if (modern) {
        std::error_code ec;
        for (const std::string& dir : {Config::modulesDir(), Config::pluginsDir(),
                                       Config::keyringDir(), Config::cacheDir()}) {
            std::filesystem::create_directories(dir, ec);
        }
        logos_core_add_modules_dir(Config::modulesDir().c_str());
        // The bundled package modules live in their own directory so that
        // logoscore's module list is byte-identical to what it reports today.
        const std::string pkgDir = paths::bundledPackageModulesDir();
        if (!pkgDir.empty())
            logos_core_add_modules_dir(pkgDir.c_str());
        if (verbose)
            fprintf(stderr, "Added session modules directory: %s\n",
                    Config::modulesDir().c_str());
    }

    // 4. Set persistence base path for module instance data
    // Config::dataDir() already reflects dirs.data, and the loader folds the
    // older persistence_path spelling into it, so there is nothing to choose
    // between here.
    std::string persistenceBase = Config::dataDir();
    logos_core_set_persistence_base_path(persistenceBase.c_str());

    // 4b. Install the access policy before any module loads. Empty =>
    //     NULL (clear). Runtime side is currently a no-op.
    logos_core_set_access_policy(
        cfg.accessPolicy.empty() ? nullptr : cfg.accessPolicy.c_str());

    // 5. Materialize per-module transport sets BEFORE logos_core_start()
    //    so capability_module (loaded inside logos_core_start) gets the
    //    listeners the operator asked for, with ephemeral ports already
    //    allocated. Each module's transports come from the
    //    `--module-transport` CLI flags, fully decoupled — nothing about
    //    core_service's listeners leaks into capability_module's. The
    //    CLI defaulted both well-known modules to a LocalSocket-only
    //    entry if the operator didn't configure them.
    auto getModuleInfos = [&moduleTransports](const std::string& name) {
        auto it = moduleTransports.find(name);
        return it == moduleTransports.end()
                   ? std::vector<TransportInfo>{}
                   : it->second;
    };

    auto coreTransportsOpt = buildTransportSet(
        getModuleInfos("core_service"), "core_service");
    if (!coreTransportsOpt) {
        fprintf(stderr,
                "Daemon startup aborted: failed to build core_service transport set "
                "(see prior log lines for which listener failed).\n");
        return 1;
    }
    LogosTransportSet coreTransports = std::move(*coreTransportsOpt);

    auto capabilityTransportsOpt = buildTransportSet(
        getModuleInfos("capability_module"), "capability_module");
    if (!capabilityTransportsOpt) {
        fprintf(stderr,
                "Daemon startup aborted: failed to build capability_module transport set "
                "(see prior log lines for which listener failed).\n");
        return 1;
    }
    LogosTransportSet capabilityTransports = std::move(*capabilityTransportsOpt);

    // Register capability_module's transports with the runtime BEFORE
    // logos_core_start launches the child subprocess. The child reads
    // the JSON via --transport-set in its argv and binds the requested child
    // provider listeners.
    {
        std::string capJson = logos::transportSetToJsonString(capabilityTransports);
        logos_core_set_module_transports("capability_module", capJson.c_str());
    }

    // 5b. core_service is liblogos': the daemon is its shell ("logoscore") and
    //     hands it what only the daemon knows. The bundled directories are what
    //     ships beside the binary, so a reserved module name resolves only there.
    std::vector<std::string> bundledDirs;
    if (!bundledDir.empty()) bundledDirs.push_back(bundledDir);
    if (modern && !paths::bundledPackageModulesDir().empty())
        bundledDirs.push_back(paths::bundledPackageModulesDir());
    for (const std::string& dir : cfg.bundledModulesDirs) {
        logos_core_add_modules_dir(dir.c_str());
        bundledDirs.push_back(dir);
    }
    std::vector<const char*> bundledList;
    for (const std::string& dir : bundledDirs) bundledList.push_back(dir.c_str());
    bundledList.push_back(nullptr);
    logos_core_set_bundled_modules_dirs(bundledList.data());
    if (!cfg.placement.empty() && logos_core_set_placement_policy(cfg.placement.c_str()) != 0) {
        fprintf(stderr, "Invalid placement policy: %s\n", cfg.placement.c_str());
        logos_core_cleanup();
        return 1;
    }
    logos_core_set_shell_identity("logoscore");
    {
        const std::string network =
            logos::transportSetToJsonString(networkTransports(coreTransports));
        logos_core_set_core_service_transports(network.c_str());
    }
    logos_core_set_operator_resolver(&resolveOperator, nullptr);
    logos_core_set_shutdown_handler(&requestShutdownFromCoreService, nullptr);
    auto* packages = new PackageService();
    {
        const std::string methods = PackageService::methods().dump();
        logos_core_set_core_service_extension(&extendCoreService, methods.c_str(), packages);
    }

    // 6. Start core (discover plugins, launch logos_host in remote mode).
    //    capability_module loads now, with the transport set we just
    //    registered.
    logos_core_start();

    // 6b. The shell binding: every lifecycle call the daemon makes goes through
    //     core_service as "logoscore". Without it there is no token authority,
    //     and nothing could load.
    logos_consumer* shell = logos_core_take_shell_binding();
    if (!shell) {
        fprintf(stderr,
                "Daemon startup aborted: no token authority. capability_module must be "
                "among the bundled modules (%s) and run in-process.\n",
                bundledDir.empty() ? "none found beside the binary" : bundledDir.c_str());
        logos_core_cleanup();
        delete packages;
        return 1;
    }
    shell_calls::install(shell);

    // -v has to reach spdlog, and it has to be set HERE.
    //
    // Module subprocesses do not share our stdio. The container gives each one
    // its own pipes, reads them line by line, and re-emits each line through
    // spdlog, picking the level from the line's prefix ("Debug:" -> debug).
    // spdlog's default is `info`, so a module's debug output was read, parsed,
    // classified -- and dropped at the last step.
    //
    // Ordering matters and cost a wrong fix: setting the level before
    // logos_core_start() is silently undone, because liblogos installs its own
    // `logos` logger during startup and that becomes the default. Set it after
    // and it sticks.
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);

    // 8. Auto-issue a fresh `auto` token for this boot.
    //
    // Tokens are hashed at rest, so every operator token (auto, alice, …)
    // validates through TokenStore::lookupByToken on demand, which core_service
    // consults through resolveOperator: issue, revoke and expiry apply at once.
    // `auto` is re-issued every boot, overwriting its hash in tokens.json and the
    // raw files at daemon/tokens/auto.json + client/auto.json.
    TokenStore tokenStore;
    const auto autoTokenOutcome = tokenStore.issueToken("auto",
                                                        /*expiresAt=*/{},
                                                        /*localOnly=*/true,
                                                        /*replace=*/true);
    if (autoTokenOutcome.status != TokenStore::IssueStatus::Ok) {
        fprintf(stderr, "Failed to auto-issue local client token (status=%d)\n",
                static_cast<int>(autoTokenOutcome.status));
        // logos_core_start() already launched the module subprocesses; leaving
        // without cleanup strands them. Every other exit from this function
        // runs logos_core_cleanup() below.
        shell_calls::release();
        logos_core_cleanup();
        delete packages;
        return 1;
    }
    const std::string autoTokenRaw = autoTokenOutcome.token;

    // 8b. Bring up the bundled package modules and point them at this
    //     session's directories.
    //
    //     These are loaded unconditionally, like basecamp does after
    //     logos_core_start (app/main.cpp), because every package command is
    //     an RPC into them — a client that had to load them first would pay
    //     the cost on its first `package` command and race any concurrent
    //     client doing the same.
    //
    //     The directory configuration is the same four calls basecamp makes
    //     (PackageCoordinator::subscribeToPackageInstallationEvents): embedded
    //     is the read-only tree beside the binary, user is the session's
    //     writable tree. Without it the manager has no writable target and
    //     scans nothing, so `package ls` would report an empty session even
    //     after a successful install.
    //
    //     Failures here are logged, not fatal: a daemon that cannot manage
    //     packages is still a perfectly good daemon for loading and calling
    //     modules, and refusing to boot would turn a missing optional module
    //     into total unavailability.
    if (modern) {
        logosctl::PlainRpcContext bootstrapRpc("core");
        bootstrapPackageModules(&bootstrapRpc, paths::bundledPackageModulesDir(),
                                cfg.signaturePolicy, verbose);
    }

    // 9. Write the live-instance state file. Carries the resolved
    //    transport endpoints (post-bind, with real ports), instanceId/
    //    pid/startedAt for co-resident clients, and a snapshot of the
    //    operator-resolved config for diagnostics. Persistent state
    //    (tokens.json) and operator preferences (config.json, only
    //    written on --persist-config) live in their own files and
    //    aren't touched here.
    DaemonRuntimeState state;
    state.instanceId    = instanceId;
    state.pid           = pid;
    state.startedAt     = currentUtcIso8601();
    state.configSource  = configSource;
    // Start from the operator-merged config so downstream consumers
    // see every preference (ssl paths, insecureTcp), then
    // overwrite the per-module map with the resolved (post-bind)
    // transports — that's the only field where state.json diverges
    // from config.json on intent.
    state.resolved              = cfg;
    state.resolved.modules.clear();
    state.resolved.modules.emplace("core_service",      toAdvertised(coreTransports));
    state.resolved.modules.emplace("capability_module", toAdvertised(capabilityTransports));
    if (!DaemonRuntimeStateFile::write(state)) {
        fprintf(stderr, "Failed to write daemon state file: %s\n",
                DaemonRuntimeStateFile::filePath().c_str());
        // logos_core_start() already launched the module subprocesses; leaving
        // without cleanup strands them. Every other exit from this function
        // runs logos_core_cleanup() below.
        shell_calls::release();
        logos_core_cleanup();
        delete packages;
        return 1;
    }

    // Persist operator preferences only if asked (legacy front-end only).
    // Done after state.json is on disk so a config that fails earlier (e.g. a
    // bind failure) doesn't pollute config.json.
    if (persistConfig) {
        if (DaemonConfigFile::write(cfg)) {
            fprintf(stdout, "Persisted config: %s\n",
                    DaemonConfigFile::filePath().c_str());
        } else {
            fprintf(stderr, "Warning: failed to persist config to %s\n",
                    DaemonConfigFile::filePath().c_str());
        }
    }

    // 10. Generate the local-client convenience artifacts (client/config.json
    //     + client/auto.json). The config.json write is gated inside
    //     writeLocalClientArtifacts on the file not already existing —
    //     operator-authored client config must not be clobbered just because a
    //     daemon happened to start in the same config dir.
    //     The one exception: an existing config.json whose instance_id
    //     no longer matches this daemon is a stale copy of our own
    //     artifact (persisted config dir, replaced daemon) and is
    //     refreshed in place — see writeLocalClientArtifacts.
    //     The raw client/auto.json is always (re)written so a config.json
    //     pointing at "auto.json" stays consistent with the freshly-issued
    //     auto token.
    if (!DaemonRuntimeStateFile::writeLocalClientArtifacts(
            instanceId, autoTokenRaw, state.startedAt,
            toAdvertised(coreTransports),
            toAdvertised(capabilityTransports),
            effectiveAccessGroup)) {
        fprintf(stderr, "Warning: failed to write local client artifacts under %s\n",
                Config::clientDir().c_str());
    }

    fprintf(stdout, "Logosctl daemon started (pid %lld, instance %s)\n",
            static_cast<long long>(pid), instanceId.c_str());
    fprintf(stdout, "Daemon state: %s\n", DaemonRuntimeStateFile::filePath().c_str());
    fprintf(stdout, "Local client config: %s\n",
            Config::clientConfigPath().c_str());
    fflush(stdout);

    // 8. The protocol runtime owns its I/O workers. The main thread only waits
    //    for a signal or the core_service shutdown RPC.
    setupSignalHandlers();
    {
        std::unique_lock<std::mutex> lock(g_shutdownMutex);
        while (!g_shutdownRequested) {
            // A POSIX signal handler may only set sig_atomic_t; the bounded
            // wait observes it without making condition_variable calls from
            // signal context.
            g_shutdownChanged.wait_for(lock, std::chrono::milliseconds(100));
        }
    }

    // 10. Cleanup
    fprintf(stdout, "Shutting down logosctl daemon...\n");
    fflush(stdout);

    DaemonRuntimeStateFile::remove();
    shell_calls::release();
    logos_core_cleanup();
    delete packages;
    LogSink::instance().stop();

    fprintf(stdout, "Logosctl daemon stopped.\n");
    fflush(stdout);

    return 0;
}
