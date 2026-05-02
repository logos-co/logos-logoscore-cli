#include "client.h"
#include "../config.h"
#include "client_state.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_instance.h>
#include <logos_transport_config.h>
#include <token_manager.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>

// ---------------------------------------------------------------------------
// RpcClient implementation — delegates all calls to daemon's core_service
// ---------------------------------------------------------------------------

struct RpcClient::Impl {
    LogosAPI* api = nullptr;
    LogosAPIClient* coreService = nullptr;
    QString instanceId;
    QString token;
    ClientState clientState;

    // Helper: invoke a core_service method and return a QJsonObject result
    template<typename... Args>
    QVariant invoke(const QString& method, Args&&... args) {
        return coreService->invokeRemoteMethod(
            QStringLiteral("core_service"), method, std::forward<Args>(args)...);
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
    // The client never opens daemon/state.json or daemon/config.json:
    // the daemon's runtime state and operator preferences aren't ours
    // to inspect. We dial whatever client/config.json says, with
    // whatever token lives in client/<token_file>. ClientStateFile::read
    // also serves an in-process override set by main.cpp when CLI
    // client-config flags were passed without --persist-config — the
    // dial spec then reflects flags + config.json + defaults for this
    // run only.
    d->clientState = ClientStateFile::read();
    if (!d->clientState.fileOk) {
        m_lastError = "No client config at " +
            QString::fromStdString(ClientStateFile::filePath()) +
            ". Either run the daemon locally first (it auto-emits a "
            "config), pass client flags + --persist-config, or write "
            "client/config.json + the matching token file by hand.";
        return false;
    }

    // Resolve token: env override wins, else read from client/<token_file>.
    d->token = Config::getToken();
    if (d->token.isEmpty())
        d->token = QString::fromStdString(
            ClientStateFile::readTokenFile(d->clientState.tokenFile));

    if (d->token.isEmpty()) {
        m_lastError = QString("No authentication token. Expected at %1 or in $LOGOSCORE_TOKEN.")
            .arg(QString::fromStdString(
                Config::clientTokenPath(QString::fromStdString(d->clientState.tokenFile)).toStdString()));
        return false;
    }

    // For LocalSocket dialing, the SDK derives the registry name from
    // `local:logos_<module>_<instance_id>`, so we need the daemon's
    // instance id. The daemon's auto-emitted client/config.json carries it;
    // remote clients (TCP / TCP-SSL) don't need it. The client never
    // opens daemon-side files — by design.
    if (!d->clientState.instanceId.empty()) {
        d->instanceId = QString::fromStdString(d->clientState.instanceId);
        qputenv("LOGOS_INSTANCE_ID", d->instanceId.toUtf8());
    }

    TokenManager::instance().saveToken("cli_client", d->token);

    // Translate ClientModuleTransport (client-side) into LogosTransportConfig
    // (SDK-side) — this is the actual dial spec.
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

    // Create LogosAPI for this CLI client. Keep the global default at
    // LocalSocket so the implicit provider in LogosAPI's ctor doesn't
    // try to bind a TLS server on the client side.
    d->api = new LogosAPI("cli_client");

    // Wire up capability_module's per-module transport. Required by the
    // SDK's auto-`requestModule` flow inside LogosAPIClient. If
    // client/config.json omits it, fall through to LocalSocket — same as the
    // SDK's global default.
    if (auto capIt = d->clientState.daemon.find("capability_module");
        capIt != d->clientState.daemon.end()) {
        d->api->setCapabilityModuleTransport(toCfg(capIt->second));
    }

    // Get a client handle to the daemon's core_service module.
    // client/config.json's per-module entry is the dial spec, full stop —
    // no env-var overrides, no daemon-advertised list to pick from.
    // If the operator wants a different transport, they re-run
    // `logoscore <subcommand> --client-transport ...` which
    // regenerates client/config.json (step 7).
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

QString RpcClient::lastError() const
{
    return m_lastError;
}

// ---------------------------------------------------------------------------
// Module lifecycle — delegate to core_service
// ---------------------------------------------------------------------------

QJsonObject RpcClient::loadModule(const QString& name)
{
    QVariant ret = d->invoke("loadModule", name);
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    // Fallback: construct error
    QJsonObject result;
    result["status"] = "error";
    result["code"] = "RPC_FAILED";
    result["message"] = QString("loadModule('%1') RPC call failed.").arg(name);
    return result;
}

QJsonObject RpcClient::unloadModule(const QString& name)
{
    QVariant ret = d->invoke("unloadModule", name);
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"] = "error";
    result["code"] = "RPC_FAILED";
    result["message"] = QString("unloadModule('%1') RPC call failed.").arg(name);
    return result;
}

QJsonObject RpcClient::reloadModule(const QString& name)
{
    QVariant ret = d->invoke("reloadModule", name);
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"] = "error";
    result["code"] = "RPC_FAILED";
    result["message"] = QString("reloadModule('%1') RPC call failed.").arg(name);
    return result;
}

// ---------------------------------------------------------------------------
// Queries — delegate to core_service
// ---------------------------------------------------------------------------

QJsonArray RpcClient::listModules(const QString& filter)
{
    QVariant ret = d->invoke("listModules", filter);
    if (ret.canConvert<QJsonArray>())
        return qvariant_cast<QJsonArray>(ret);
    return {};
}

QJsonObject RpcClient::getStatus()
{
    QVariant ret = d->invoke("getStatus");
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    // If the RPC failed, we don't have detailed daemon info to fall
    // back on — the client never opens daemon-side files by design, so pid /
    // started_at / advertised modules are not in our hands. Surface
    // what we know (the instance_id from client/config.json, if local) and
    // tag the response as RPC-degraded so callers exit non-zero.
    QJsonObject status;
    QJsonObject daemon;
    daemon["status"] = "not_running";
    if (!d->instanceId.isEmpty())
        daemon["instance_id"] = d->instanceId;
    daemon["version"] = QCoreApplication::applicationVersion();
    status["daemon"] = daemon;
    status["modules"] = QJsonArray();
    status["rpc_error"] = "core_service not reachable";
    return status;
}

QJsonObject RpcClient::getModuleInfo(const QString& name)
{
    QVariant ret = d->invoke("getModuleInfo", name);
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"] = "error";
    result["code"] = "RPC_FAILED";
    result["message"] = QString("getModuleInfo('%1') RPC call failed.").arg(name);
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

QJsonObject RpcClient::callModuleMethod(const QString& module,
                                         const QString& method,
                                         const QVariantList& args)
{
    QVariant ret = d->invoke("callModuleMethod", module, method, args);
    if (ret.canConvert<QJsonObject>())
        return ret.toJsonObject();

    QJsonObject result;
    result["status"] = "error";
    result["code"] = "RPC_FAILED";
    result["message"] = QString("callModuleMethod('%1', '%2') RPC call failed.").arg(module, method);
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
    result["status"] = "error";
    result["code"] = "RPC_FAILED";
    result["message"] = "shutdown RPC call failed.";
    return result;
}

// ---------------------------------------------------------------------------
// Event watching — uses SDK directly for real-time events
// ---------------------------------------------------------------------------

bool RpcClient::watchModuleEvents(const QString& module,
                                   const QString& eventName,
                                   std::function<void(const QJsonObject&)> callback)
{
    if (!m_connected)
        return false;

    // First, tell core_service to start forwarding events from the target module
    QVariant subscribed = d->invoke("watchModuleEvents", module, eventName);
    if (!subscribed.toBool())
        return false;

    // Subscribe to core_service's forwarded "module_event" events
    LogosObject* obj = d->coreService->requestObject("core_service");
    if (!obj)
        return false;

    d->coreService->onEvent(obj, QStringLiteral("module_event"),
        [module, callback](const QString& event, const QVariantList& data) {
            Q_UNUSED(event);
            // data format: [sourceModule, eventName, ...eventData]
            if (data.size() < 2)
                return;
            QString srcModule = data.at(0).toString();
            if (srcModule != module)
                return;

            QJsonObject eventObj;
            eventObj["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            eventObj["module"] = srcModule;
            eventObj["event"] = data.at(1).toString();

            QJsonObject eventData;
            for (int i = 2; i < data.size(); ++i) {
                eventData[QString("arg%1").arg(i - 2)] = QJsonValue::fromVariant(data.at(i));
            }
            eventObj["data"] = eventData;
            callback(eventObj);
        });

    return true;
}
