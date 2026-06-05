#include "client.h"
#include "../config.h"
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
    nlohmann::json invoke(const std::string& method,
                          const nlohmann::json& args = nlohmann::json::array()) {
        return coreService->invokeRemoteMethod("core_service", method, args);
    }
};

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
            ". Either run the daemon locally first (it auto-emits a "
            "config), pass client flags + --persist-config, or write "
            "client/config.json + the matching token file by hand.";
        return false;
    }

    // Resolve token: env override wins, else read from client/<token_file>.
    d->token = Config::getToken();
    if (d->token.empty())
        d->token = ClientStateFile::readTokenFile(d->clientState.tokenFile);

    if (d->token.empty()) {
        m_lastError = fmt::format(
            "No authentication token. Expected at {} or in $LOGOSCORE_TOKEN.",
            Config::clientTokenPath(d->clientState.tokenFile));
        return false;
    }

    // For LocalSocket dialing, the SDK derives the registry name from
    // `local:logos_<module>_<instance_id>`, so we need the daemon's
    // instance id. The daemon's auto-emitted client/config.json carries it;
    // remote clients (TCP / TCP-SSL) don't need it.
    if (!d->clientState.instanceId.empty()) {
        d->instanceId = d->clientState.instanceId;
        setenv("LOGOS_INSTANCE_ID", d->instanceId.c_str(), 1);
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
        m_lastError = "client/config.json: 'daemon.core_service' is required.";
        return false;
    }
    const LogosTransportConfig coreServiceCfg = toCfg(coreIt->second);

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

LogosMap RpcClient::unloadModule(const std::string& name)
{
    nlohmann::json ret = d->invoke("unloadModule", nlohmann::json::array({name}));
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message", fmt::format("unloadModule('{}') RPC call failed.", name)}};
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

LogosList RpcClient::listModules(const std::string& filter)
{
    nlohmann::json ret = d->invoke("listModules", nlohmann::json::array({filter}));
    if (ret.is_array()) return ret;
    return LogosList::array();
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

LogosList RpcClient::getModuleStats()
{
    nlohmann::json ret = d->invoke("getModuleStats");
    if (ret.is_array()) return ret;
    return LogosList::array();
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
    nlohmann::json ret = d->invoke("shutdown");
    if (ret.is_object()) return ret;
    return LogosMap{{"status","error"},{"code","RPC_FAILED"},
                    {"message","shutdown RPC call failed."}};
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
            gmtime_r(&tt, &utc);
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
