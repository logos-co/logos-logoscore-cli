#include "daemon.h"
#include "daemon_state.h"
#include "port_allocator.h"
#include "token_store.h"
#include "../config.h"
#include "../paths.h"
#include "logos_core.h"

#include <logos_api.h>
#include <logos_api_provider.h>
#include <logos_transport_config.h>
#include <logos_transport_config_json.h>
#include <token_manager.h>
#include "../core_service/core_service_impl.h"

#include <QCoreApplication>
#include <QDebug>
#include <QString>

#include <uuid.h>

#include <csignal>
#include <cstdint>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

static volatile sig_atomic_t g_shutdownRequested = 0;

void Daemon::signalHandler(int signal)
{
    (void)signal;
    g_shutdownRequested = 1;

    // Post a quit event to the Qt event loop
    if (QCoreApplication::instance())
        QCoreApplication::quit();
}

void Daemon::setupSignalHandlers()
{
    struct sigaction sa;
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

namespace {

// Materialize a per-module TransportInfo list into a LogosTransportSet
// that the SDK can hand to LogosAPI. For non-LocalSocket entries with
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
        if      (eff.protocol == "tcp")     c.protocol = LogosProtocol::Tcp;
        else if (eff.protocol == "tcp_ssl") c.protocol = LogosProtocol::TcpSsl;
        else                                c.protocol = LogosProtocol::LocalSocket;
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

} // namespace

int Daemon::start(int argc, char* argv[],
                  const DaemonConfig& cfg,
                  const std::string& configSource,
                  bool persistConfig)
{
    const auto& modulesDirs      = cfg.modulesDirs;
    const auto& persistencePath  = cfg.persistencePath;
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

    setenv("LOGOS_INSTANCE_ID", instanceId.c_str(), 1);

    // 2. Initialize logos core
    logos_core_init(argc, argv);

    // 3. Add plugin directories — user-specified and bundled
    //    Resolve to absolute paths: logos_core cannot load plugin metadata from relative paths.
    for (const std::string& dir : modulesDirs) {
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

    // 4. Set persistence base path for module instance data
    std::string persistenceBase = persistencePath.empty()
        ? Config::configDir().toStdString() + "/data"
        : persistencePath;
    logos_core_set_persistence_base_path(persistenceBase.c_str());

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
    // the JSON via --transport-set in its argv and uses the explicit-
    // transport LogosAPI constructor so its provider binds every
    // listener.
    {
        std::string capJson = logos::transportSetToJsonString(capabilityTransports);
        logos_core_set_module_transports("capability_module", capJson.c_str());
    }

    // 6. Start core (discover plugins, launch logos_host in remote mode).
    //    capability_module loads now, with the transport set we just
    //    registered.
    logos_core_start();

    // 7. Register core_service as an in-process module via the C++ SDK.
    //    core_service can publish on multiple transports simultaneously:
    //    a local QLocalSocket (back-compat) + any TCP / TCP+SSL listeners
    //    specified via --transport.
    auto* coreServiceApi = new LogosAPI(QString("core_service"), coreTransports);
    auto* coreServiceImpl = new CoreServiceImpl();

    coreServiceImpl->init(coreServiceApi);
    auto* provider = coreServiceApi->getProvider();
    provider->registerObject("core_service", static_cast<LogosProviderObject*>(coreServiceImpl));

    // 8. Auto-issue a fresh `auto` token for this boot.
    //
    // Daemons store hashes at rest, so the raw value of any operator-
    // issued token (alice, bob, …) isn't recoverable on restart — those
    // tokens validate via TokenStore::lookupByToken on demand. The `auto`
    // token is special: the daemon (re-)generates it every boot,
    // overwrites both the hash entry in tokens.json and the raw files at
    // daemon/tokens/auto.json + client/auto.json, and registers the raw
    // version with the in-process TokenManager so the hot path stays
    // fast (no per-RPC hash + DB lookup). `local_only=true` keeps a
    // leaked client/auto.json from being usable over TCP from a remote
    // host. Operator-issued tokens take effect only on the next daemon
    // restart (or future SIGHUP-driven reload).
    // Capture the pre-issue tokens-file state to gate the local-client
    // config auto-emit below. A daemon launching into an empty config
    // dir gets a generated client/config.json + auto.json so `status`
    // works out of the box; a returning daemon (tokens.json already
    // had operator-issued entries) leaves client/config.json alone —
    // the operator has shown they're managing the client side.
    const bool tokensFileWasMissing =
        !std::filesystem::exists(Config::daemonTokensPath().toStdString());

    TokenStore tokenStore(Config::configDir().toStdString());
    auto autoTokenRaw = tokenStore.issueToken("auto", /*expiresAt=*/{},
                                              /*localOnly=*/true,
                                              /*replace=*/true);
    if (!autoTokenRaw) {
        fprintf(stderr, "Failed to auto-issue local client token\n");
        return 1;
    }

    TokenManager::instance().saveToken("cli_client",
                                       QString::fromStdString(*autoTokenRaw));

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
    // see every preference (loadModules, ssl paths, insecureTcp), then
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
        return 1;
    }

    // Persist operator preferences only if asked. Done after state.json
    // is on disk so a config that fails earlier (e.g. bind failure)
    // doesn't pollute config.json. The persisted file holds intent
    // (port=0 stays 0) — the resolved values are in state.json.
    // Persistence failures are non-fatal: log and continue.
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
    //     remote clients are expected to write their own, and an
    //     operator-authored remote config must not be clobbered just
    //     because a daemon happened to start in the same config dir.
    //     The raw client/auto.json is always (re)written so a config.json
    //     pointing at "auto.json" stays consistent with the freshly-issued
    //     auto token.
    if (!DaemonRuntimeStateFile::writeLocalClientArtifacts(
            instanceId, *autoTokenRaw, state.startedAt, tokensFileWasMissing)) {
        fprintf(stderr, "Warning: failed to write local client artifacts under %s\n",
                Config::clientDir().toStdString().c_str());
    }

    fprintf(stdout, "Logoscore daemon started (pid %lld, instance %s)\n",
            static_cast<long long>(pid), instanceId.c_str());
    fprintf(stdout, "Daemon state: %s\n", DaemonRuntimeStateFile::filePath().c_str());
    fprintf(stdout, "Local client config: %s\n",
            Config::clientConfigPath().toStdString().c_str());
    fflush(stdout);

    // 8. Set up signal handlers for clean shutdown. SIGINT / SIGTERM
    //    fire `QCoreApplication::quit()`, which makes `exec()` return
    //    and the explicit cleanup below runs. We also subscribe to
    //    `aboutToQuit` as a defense-in-depth: any future code path
    //    that calls `quit()` without going through the explicit
    //    cleanup (e.g. an exception caught by the event loop) still
    //    unlinks state.json so a co-resident client can detect
    //    "no live daemon".
    setupSignalHandlers();
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     []() { DaemonRuntimeStateFile::remove(); });

    // 9. Run Qt event loop (blocks)
    int result = QCoreApplication::exec();

    // 10. Cleanup
    fprintf(stdout, "Shutting down logoscore daemon...\n");
    fflush(stdout);

    logos_core_cleanup();
    DaemonRuntimeStateFile::remove();

    delete coreServiceImpl;
    delete coreServiceApi;

    fprintf(stdout, "Logoscore daemon stopped.\n");
    fflush(stdout);

    return result;
}
