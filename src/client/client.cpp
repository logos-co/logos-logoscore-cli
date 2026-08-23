#include "client.h"
#include "../config.h"
#include "../daemon/daemon_state.h"
#include "../local_endpoint.h"
#include "../platform_compat.h"
#include "../process_util.h"
#include "client_state.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_instance.h>
#include <logos_transport_config.h>
#include <token_manager.h>

#include <QCoreApplication>

#include <fmt/format.h>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <thread>

// ---------------------------------------------------------------------------
// RpcClient implementation — delegates all calls to daemon's core_service
// ---------------------------------------------------------------------------

struct RpcClient::Impl {
    LogosAPI* api = nullptr;
    LogosAPIClient* coreService = nullptr;
    std::string instanceId;
    std::string token;
    ClientState clientState;

    // Helper: invoke a core_service method via the nlohmann::json overload.
    //
    // `timeoutMs` exists because the transport's default deadline is 20
    // seconds (logos::Timeout), which is right for a status query and far too
    // short for anything that touches the network. An install that outran it
    // reported "RPC call failed" while the daemon carried on and finished the
    // job -- the package landed on disk and the user was told it had not.
    nlohmann::json invoke(const std::string& method,
                          const nlohmann::json& args = nlohmann::json::array(),
                          int timeoutMs = 0) {
        if (timeoutMs > 0) {
            return coreService->invokeRemoteMethod("core_service", method, args,
                                                   Timeout(timeoutMs));
        }
        return coreService->invokeRemoteMethod("core_service", method, args);
    }
};

namespace {

// Deadlines for the operations that leave the machine. The transport's default
// is 20 seconds -- fine for "is the daemon up", useless for "fetch and install
// a blockchain node". These are generous on purpose: the cost of waiting too
// long is a slow command, the cost of waiting too little is telling someone
// their install failed while it is still running and about to succeed.
constexpr int kCatalogTimeoutMs  = 2  * 60 * 1000;   // resolve against the catalog
constexpr int kTransferTimeoutMs = 30 * 60 * 1000;   // download + install

// Deadlines for `shutdown`, which is the one call whose reply is expected to
// go missing -- see RpcClient::shutdown below.
//
// The daemon answers shutdown before doing any work (the handler fills in two
// fields and returns), so a reply still absent after this long is a reply that
// is not coming; the 20s default only makes the operator wait for it.
constexpr int kShutdownTimeoutMs = 5 * 1000;
// Having stopped waiting for words, how long to watch for the deed. Generous:
// a clean shutdown unloads every module, which is one subprocess apiece.
constexpr int kShutdownConfirmMs = 15 * 1000;
// Per-probe budget when the daemon is remote and there is no pid to watch.
constexpr int kShutdownProbeMs   = 1500;
constexpr int kShutdownProbeGapMs = 200;

} // namespace

RpcClient::RpcClient()
    : d(new Impl)
{
}

RpcClient::~RpcClient()
{
    delete d->api;
    delete d;
}

bool RpcClient::connect()
{
    // Read client config (pure parse — doesn't probe the daemon).
    d->clientState = ClientStateFile::read();
    if (!d->clientState.fileOk) {
        m_lastError = "No client config at " +
            ClientStateFile::filePath() +
            ". Start a daemon in this session (it writes one on boot), or "
            "install a dial spec with `logosctl client config set FILE` "
            "alongside the matching token file.";
        return false;
    }

    // Resolve token: env override wins, else read from client/<token_file>.
    d->token = Config::getToken();
    if (d->token.empty())
        d->token = ClientStateFile::readTokenFile(d->clientState.tokenFile);

    if (d->token.empty()) {
        m_lastError = fmt::format(
            "No authentication token. Expected at {} or in $LOGOSCTL_TOKEN.",
            Config::clientTokenPath(d->clientState.tokenFile));
        return false;
    }

    // For LocalSocket dialing, the SDK derives the registry name from
    // `local:logos_<module>_<instance_id>`, so we need the daemon's
    // instance id. The daemon's auto-emitted client/config.json carries it;
    // remote clients (TCP / TCP-SSL) don't need it.
    if (!d->clientState.instanceId.empty()) {
        d->instanceId = d->clientState.instanceId;
        logosctl::setEnvVar("LOGOS_INSTANCE_ID", d->instanceId.c_str());
    }

    // Keep the legacy self-identity entry (the CLI's LogosAPI is named
    // "cli_client", so some code paths look it up by that name).
    TokenManager::instance().saveToken("cli_client", d->token);
    TokenManager::instance().saveToken("core_service", d->token);

    // Translate ClientModuleTransport (client-side) into LogosTransportConfig
    auto toCfg = [](const ClientModuleTransport& t) {
        LogosTransportConfig cfg;
        if      (t.protocol == "tcp")     cfg.protocol = LogosProtocol::Tcp;
        else if (t.protocol == "tcp_ssl") cfg.protocol = LogosProtocol::TcpSsl;
        else                              cfg.protocol = LogosProtocol::LocalSocket;
        cfg.host       = t.host;
        cfg.port       = t.port;
        cfg.caFile     = t.caFile;
        cfg.verifyPeer = t.verifyPeer;
        cfg.codec = (t.codec == "cbor") ? LogosWireCodec::Cbor : LogosWireCodec::Json;
        return cfg;
    };

    // core_service is mandatory.
    auto coreIt = d->clientState.daemon.find("core_service");
    if (coreIt == d->clientState.daemon.end()) {
        m_lastError = ClientStateFile::filePath() + ": 'daemon.core_service' is required.";
        return false;
    }
    const LogosTransportConfig coreServiceCfg = toCfg(coreIt->second);

    // A local dial with nothing at the other end fails HERE, rather than
    // twenty seconds into the first RPC.
    //
    // Everything above this line is a parse; none of it can tell whether the
    // daemon is there, and neither can the LogosAPIClient built below --
    // getClient() hands back a handle whether or not anyone is listening. So
    // ask the socket itself: is it missing, or does it refuse us? That catches
    // the session a daemon left behind when it stopped CLEANLY, which the pid
    // check in Command::ensureConnected() cannot see -- that path removes
    // daemon/state.json, so there is no pid left to find dead.
    //
    // Only for LocalSocket, and only when the answer is a definite no. A tcp /
    // tcp_ssl dial has no socket to look at, and localEndpointProvablyAbsent()
    // fails closed on everything short of proof, so a reachable daemon is
    // never refused on a guess. See src/local_endpoint.h.
    if (coreServiceCfg.protocol == LogosProtocol::LocalSocket) {
        std::string endpoint;
        if (logosctl::localEndpointProvablyAbsent("core_service", d->instanceId,
                                                  &endpoint)) {
            m_lastError = fmt::format(
                "No daemon running (no local endpoint at {}). A daemon that "
                "stopped removes it; start one in this session to get it back.",
                endpoint);
            return false;
        }
    }

    d->api = new LogosAPI("cli_client");

    // Wire up capability_module's per-module transport.
    if (auto capIt = d->clientState.daemon.find("capability_module");
        capIt != d->clientState.daemon.end()) {
        d->api->setCapabilityModuleTransport(toCfg(capIt->second));
    }

    d->coreService = d->api->getClient("core_service", coreServiceCfg);
    if (!d->coreService) {
        m_lastError = "Failed to get core_service client handle.";
        return false;
    }

    m_connected = true;
    return true;
}

bool RpcClient::isConnected() const
{
    return m_connected;
}

std::string RpcClient::lastError() const
{
    return m_lastError;
}

// ---------------------------------------------------------------------------
// Module lifecycle — delegate to core_service
// ---------------------------------------------------------------------------

LogosMap RpcClient::loadModule(const std::string& name)
{
    nlohmann::json ret = d->invoke("loadModule", nlohmann::json::array({name}));
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("loadModule('{}') RPC call failed.", name)}};
}

LogosMap RpcClient::unloadModule(const std::string& name, bool withDependents)
{
    nlohmann::json ret = d->invoke("unloadModule",
                                   nlohmann::json::array({name, withDependents}));
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("unloadModule('{}') RPC call failed.", name)}};
}

LogosMap RpcClient::refreshModules()
{
    nlohmann::json ret = d->invoke("refreshModules", nlohmann::json::array());
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", "refreshModules() RPC call failed."}};
}

// ---------------------------------------------------------------------------
// Package operations — delegate to core_service
// ---------------------------------------------------------------------------

LogosMap RpcClient::planPackageOperation(const std::string& op, const LogosList& names,
                                          const LogosMap& opts)
{
    // Resolving a plan reads the catalog, so it is network-bound too -- less
    // so than the install itself, hence the smaller budget.
    nlohmann::json ret = d->invoke("planPackageOperation",
                                   nlohmann::json::array({op, names, opts}),
                                   kCatalogTimeoutMs);
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("planPackageOperation('{}') RPC call failed.", op)}};
}

LogosMap RpcClient::applyPackageOperation(const std::string& op, const LogosList& names,
                                           const LogosMap& opts)
{
    // Installs pull from the network and can take minutes on a cold catalog;
    // the default 20s deadline is far too short for that. This comment used to
    // sit above a call that passed no timeout at all.
    nlohmann::json ret = d->invoke("applyPackageOperation",
                                   nlohmann::json::array({op, names, opts}),
                                   kTransferTimeoutMs);
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("applyPackageOperation('{}') RPC call failed.", op)}};
}

LogosMap RpcClient::downloadPackage(const std::string& name, const LogosMap& opts)
{
    // Same deadline reasoning as applyPackageOperation: this is a network
    // fetch, not a local query.
    nlohmann::json ret = d->invoke("downloadPackage",
                                   nlohmann::json::array({name, opts}),
                                   kTransferTimeoutMs);
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("downloadPackage('{}') RPC call failed.", name)}};
}

LogosMap RpcClient::reloadModule(const std::string& name)
{
    nlohmann::json ret = d->invoke("reloadModule", nlohmann::json::array({name}));
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("reloadModule('{}') RPC call failed.", name)}};
}

// ---------------------------------------------------------------------------
// Queries — delegate to core_service
// ---------------------------------------------------------------------------

std::optional<LogosList> RpcClient::listModules(const std::string& filter)
{
    nlohmann::json ret = d->invoke("listModules", nlohmann::json::array({filter}));
    if (ret.is_array()) return ret;
    return std::nullopt;   // no reply -- NOT an empty module list
}

LogosMap RpcClient::getStatus()
{
    nlohmann::json ret = d->invoke("getStatus");
    if (ret.is_object()) return ret;

    std::string version = QCoreApplication::applicationVersion().toStdString();
    LogosMap daemon{{"status","not_running"},{"version", version}};
    if (!d->instanceId.empty())
        daemon["instance_id"] = d->instanceId;
    return LogosMap{{"daemon", daemon},
                    {"modules", LogosList::array()},
                    {"rpc_error", "core_service not reachable"}};
}

LogosMap RpcClient::getModuleInfo(const std::string& name)
{
    nlohmann::json ret = d->invoke("getModuleInfo", nlohmann::json::array({name}));
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("getModuleInfo('{}') RPC call failed.", name)}};
}

std::optional<LogosList> RpcClient::getModuleStats()
{
    nlohmann::json ret = d->invoke("getModuleStats");
    if (ret.is_array()) return ret;
    return std::nullopt;   // no reply -- NOT "no modules to report on"
}

// ---------------------------------------------------------------------------
// Proxied call — delegate to core_service
// ---------------------------------------------------------------------------

LogosMap RpcClient::callModuleMethod(const std::string& module,
                                      const std::string& method,
                                      const LogosList& args)
{
    nlohmann::json ret = d->invoke("callModuleMethod",
                                   nlohmann::json::array({module, method, args}));
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("callModuleMethod('{}','{}') RPC call failed.",
                                            module, method)}};
}

// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

LogosMap RpcClient::shutdown()
{
    // Snapshot the daemon's pid BEFORE asking it to die. A clean shutdown
    // removes daemon/state.json, so reading the file afterwards cannot tell
    // "it exited" apart from "it was never there".
    //
    // Only if it is *this* daemon's state file. A session directory can hold
    // a co-resident daemon's state while the client dials a remote one (or a
    // stale file from a daemon that crashed), and watching an unrelated --
    // possibly long-dead -- pid would turn "gone" into a foregone conclusion.
    // The instance id is written into daemon/state.json and client/config.yaml
    // by the same boot, so equality means one daemon; a hand-written remote
    // dial spec carries no instance id and never matches.
    //
    // And only if it is alive right now: a pid that was already dead before we
    // said anything proves nothing about what our request did, so it must not
    // become the evidence that our request worked. (Command::ensureConnected
    // refuses such a session outright, so `logosctl stop` never gets here;
    // this keeps the reasoning sound for every other caller.)
    const DaemonRuntimeState before = DaemonRuntimeStateFile::read();
    const bool watchable = before.fileOk
                        && before.pid > 0
                        && !before.instanceId.empty()
                        && before.instanceId == d->instanceId
                        && logosctl::processAlive(before.pid);
    const long long pid = watchable ? before.pid : -1;

    nlohmann::json ret = d->invoke("shutdown", nlohmann::json::array(),
                                   kShutdownTimeoutMs);
    if (ret.is_object()) return ret;

    // No reply. Every other call in this file is right to read that as
    // failure; this one is not. Asking a process to die is the one request
    // whose answer races its own delivery -- the daemon replies, then leaves
    // its event loop, and anything still sitting in the socket's write buffer
    // when the loop goes away is lost with it. docs/spec.md has always
    // described the intended behaviour ("If the daemon exits before the RPC
    // response arrives (expected behavior), the client treats the connection
    // loss as a successful shutdown") and docs/project.md has always promised
    // exit code 0 for it. The code did neither: it reported RPC_FAILED, and
    // `logosctl daemon stop` exited 3, for shutdowns that had succeeded.
    //
    // Reporting success unconditionally would be the opposite mistake, and
    // the more dangerous one: a daemon wedged badly enough not to answer also
    // does not answer. So ask the question the missing reply was standing in
    // for, and answer it from evidence.
    std::string how;
    if (confirmDaemonStopped(pid, how)) {
        return LogosMap{{"status",  "ok"},
                        {"message", "Daemon shutting down."},
                        // Present only on this path, so `--json` output stays
                        // byte-identical whenever the reply did arrive.
                        {"confirmed_by", how}};
    }

    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", pid > 0
                        ? fmt::format("shutdown RPC returned no reply and the daemon "
                                      "(pid {}) is still running {}s later.",
                                      pid, kShutdownConfirmMs / 1000)
                        : fmt::format("shutdown RPC returned no reply and the daemon "
                                      "is still answering {}s later.",
                                      kShutdownConfirmMs / 1000)}};
}

bool RpcClient::confirmDaemonStopped(long long pid, std::string& how)
{
    // Co-resident daemon: we have its pid, so watch the process itself.
    // That is proof rather than inference -- it cannot be fooled by a busy
    // event loop, a dropped socket, or a reply we simply failed to read.
    if (pid > 0) {
        if (logosctl::waitForProcessExit(pid, kShutdownConfirmMs)) {
            how = "process-exit";
            return true;
        }
        return false;
    }

    // Remote daemon, or a session whose state file we cannot see: no pid to
    // watch, so put the question to the endpoint instead. A daemon that still
    // serves getStatus plainly did not stop; one that has stopped answering is
    // gone as far as this client is concerned, which is the only sense in
    // which "gone" means anything across a network.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(kShutdownConfirmMs);
    for (;;) {
        if (!d->invoke("getStatus", nlohmann::json::array(),
                       kShutdownProbeMs).is_object()) {
            how = "endpoint-unreachable";
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kShutdownProbeGapMs));
    }
}

// ---------------------------------------------------------------------------
// Event watching — uses SDK directly for real-time events
// ---------------------------------------------------------------------------

bool RpcClient::watchModuleEvents(const std::string& module,
                                   const std::string& eventName,
                                   std::function<void(const LogosMap&)> callback)
{
    if (!m_connected)
        return false;

    nlohmann::json subscribed = d->invoke("watchModuleEvents",
                                          nlohmann::json::array({module, eventName}));
    if (!subscribed.is_boolean() || !subscribed.get<bool>())
        return false;

    LogosObject* obj = d->coreService->requestObject("core_service");
    if (!obj)
        return false;

    d->coreService->onEvent(obj, std::string("module_event"),
        [module, callback](const std::string& /*event*/, const nlohmann::json& data) {
            if (!data.is_array() || data.size() < 2)
                return;
            if (!data[0].is_string() || data[0].get<std::string>() != module)
                return;

            // Build ISO timestamp without Qt date helpers to avoid Qt includes
            auto now = std::chrono::system_clock::now();
            std::time_t tt = std::chrono::system_clock::to_time_t(now);
            struct tm utc{};
            logosctl::gmtimeR(&tt, &utc);
            char tsBuf[32];
            std::strftime(tsBuf, sizeof(tsBuf), "%Y-%m-%dT%H:%M:%SZ", &utc);

            LogosMap eventObj;
            eventObj["timestamp"] = std::string(tsBuf);
            eventObj["module"]    = data[0];
            eventObj["event"]     = data[1];

            LogosMap eventData;
            for (size_t i = 2; i < data.size(); ++i)
                eventData["arg" + std::to_string(i - 2)] = data[i];
            eventObj["data"] = eventData;
            callback(eventObj);
        });

    return true;
}
