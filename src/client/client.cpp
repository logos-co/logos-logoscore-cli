#include "client.h"
#include "../config.h"
#include "client_state.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_instance.h>
#include <logos_transport_config.h>
#include <token_manager.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <fmt/format.h>
#include <cstdlib>

// ---------------------------------------------------------------------------
// RpcClient implementation — delegates all calls to daemon's core_service
// ---------------------------------------------------------------------------

struct RpcClient::Impl {
    LogosAPI* api = nullptr;
    LogosAPIClient* coreService = nullptr;
    std::string instanceId;
    std::string token;
    ClientState clientState;

    // Helper: invoke a core_service method and return a QVariant result.
    // String args must be passed as QString (QVariant has no std::string ctor).
    template<typename... Args>
    QVariant invoke(const char* method, Args&&... args) {
        return coreService->invokeRemoteMethod(
            QString("core_service"), QString(method), std::forward<Args>(args)...);
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

    TokenManager::instance().saveToken("cli_client", d->token);

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

QJsonObject RpcClient::loadModule(const std::string& name)
{
    QVariant ret = d->invoke("loadModule", QString::fromStdString(name));
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"]  = "error";
    result["code"]    = "RPC_FAILED";
    result["message"] = QString::fromStdString(
        fmt::format("loadModule('{}') RPC call failed.", name));
    return result;
}

QJsonObject RpcClient::unloadModule(const std::string& name)
{
    QVariant ret = d->invoke("unloadModule", QString::fromStdString(name));
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"]  = "error";
    result["code"]    = "RPC_FAILED";
    result["message"] = QString::fromStdString(
        fmt::format("unloadModule('{}') RPC call failed.", name));
    return result;
}

QJsonObject RpcClient::reloadModule(const std::string& name)
{
    QVariant ret = d->invoke("reloadModule", QString::fromStdString(name));
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"]  = "error";
    result["code"]    = "RPC_FAILED";
    result["message"] = QString::fromStdString(
        fmt::format("reloadModule('{}') RPC call failed.", name));
    return result;
}

// ---------------------------------------------------------------------------
// Queries — delegate to core_service
// ---------------------------------------------------------------------------

QJsonArray RpcClient::listModules(const std::string& filter)
{
    QVariant ret = d->invoke("listModules", QString::fromStdString(filter));
    if (ret.canConvert<QJsonArray>())
        return qvariant_cast<QJsonArray>(ret);
    return {};
}

QJsonObject RpcClient::getStatus()
{
    QVariant ret = d->invoke("getStatus");
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject status;
    QJsonObject daemon;
    daemon["status"] = "not_running";
    if (!d->instanceId.empty())
        daemon["instance_id"] = QString::fromStdString(d->instanceId);
    daemon["version"] = QCoreApplication::applicationVersion();
    status["daemon"]    = daemon;
    status["modules"]   = QJsonArray();
    status["rpc_error"] = "core_service not reachable";
    return status;
}

QJsonObject RpcClient::getModuleInfo(const std::string& name)
{
    QVariant ret = d->invoke("getModuleInfo", QString::fromStdString(name));
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"]  = "error";
    result["code"]    = "RPC_FAILED";
    result["message"] = QString::fromStdString(
        fmt::format("getModuleInfo('{}') RPC call failed.", name));
    return result;
}

QJsonArray RpcClient::getModuleStats()
{
    QVariant ret = d->invoke("getModuleStats");
    if (ret.canConvert<QJsonArray>())
        return qvariant_cast<QJsonArray>(ret);
    return {};
}

// ---------------------------------------------------------------------------
// Proxied call — delegate to core_service
// ---------------------------------------------------------------------------

QJsonObject RpcClient::callModuleMethod(const std::string& module,
                                         const std::string& method,
                                         const QVariantList& args)
{
    QVariant ret = d->invoke("callModuleMethod",
                             QString::fromStdString(module),
                             QString::fromStdString(method),
                             args);
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"]  = "error";
    result["code"]    = "RPC_FAILED";
    result["message"] = QString::fromStdString(
        fmt::format("callModuleMethod('{}', '{}') RPC call failed.", module, method));
    return result;
}

// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

QJsonObject RpcClient::shutdown()
{
    QVariant ret = d->invoke("shutdown");
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"]  = "error";
    result["code"]    = "RPC_FAILED";
    result["message"] = "shutdown RPC call failed.";
    return result;
}

// ---------------------------------------------------------------------------
// Event watching — uses SDK directly for real-time events
// ---------------------------------------------------------------------------

bool RpcClient::watchModuleEvents(const std::string& module,
                                   const std::string& eventName,
                                   std::function<void(const QJsonObject&)> callback)
{
    if (!m_connected)
        return false;

    QVariant subscribed = d->invoke("watchModuleEvents",
                                    QString::fromStdString(module),
                                    QString::fromStdString(eventName));
    if (!subscribed.toBool())
        return false;

    LogosObject* obj = d->coreService->requestObject("core_service");
    if (!obj)
        return false;

    d->coreService->onEvent(obj, "module_event",
        [module, callback](const QString& event, const QVariantList& data) {
            Q_UNUSED(event);
            if (data.size() < 2)
                return;
            if (data.at(0).toString().toStdString() != module)
                return;

            QJsonObject eventObj;
            eventObj["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            eventObj["module"]    = data.at(0).toString();
            eventObj["event"]     = data.at(1).toString();

            QJsonObject eventData;
            for (int i = 2; i < data.size(); ++i)
                eventData[QString("arg%1").arg(i - 2)] = QJsonValue::fromVariant(data.at(i));
            eventObj["data"] = eventData;
            callback(eventObj);
        });

    return true;
}
