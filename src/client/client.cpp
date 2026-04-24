#include "client.h"
#include "../config.h"
#include "../daemon/connection_file.h"
#include "client_connection.h"

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
    ConnectionInfo connInfo;

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
    // Read connection file (pure parse — doesn't probe the daemon).
    d->connInfo = ConnectionFile::read();
    if (!d->connInfo.fileOk) {
        m_lastError = "No running logoscore daemon. Start one with: logoscore -D";
        return false;
    }

    d->instanceId = QString::fromStdString(d->connInfo.instanceId);

    // Resolve token
    d->token = Config::getToken();
    if (d->token.isEmpty())
        d->token = QString::fromStdString(d->connInfo.token);

    if (d->token.isEmpty()) {
        m_lastError = "No authentication token found.";
        return false;
    }

    // Set LOGOS_INSTANCE_ID so LogosInstance::id() returns the correct registry URL
    qputenv("LOGOS_INSTANCE_ID", d->instanceId.toUtf8());

    // Store token in TokenManager so the SDK can authenticate
    TokenManager::instance().saveToken("cli_client", d->token);

    // Pick a transport from those the daemon advertised:
    //   - $LOGOSCORE_CLIENT_TRANSPORT (set by main.cpp from --client-transport)
    //     if present and matches one of the advertised;
    //   - otherwise "local" if the daemon advertised it;
    //   - otherwise error (no viable transport).
    // Back-compat: if the daemon didn't advertise any `transports` block
    // (older daemon), fall back to the default LocalSocket path.
    if (!d->connInfo.transports.empty()) {
        const std::string preferredCstr =
            qEnvironmentVariable("LOGOSCORE_CLIENT_TRANSPORT", "local").toStdString();
        const TransportInfo* chosen = nullptr;
        for (const auto& t : d->connInfo.transports) {
            if (t.protocol == preferredCstr) { chosen = &t; break; }
        }
        if (!chosen) {
            QStringList avail;
            for (const auto& t : d->connInfo.transports)
                avail.append(QString::fromStdString(t.protocol));
            m_lastError = QString("Transport '%1' not offered by daemon. Available: %2")
                              .arg(QString::fromStdString(preferredCstr), avail.join(", "));
            return false;
        }

        // Apply --client-tcp-host / --client-tcp-port / --no-verify-peer
        // overrides. The endpoint the client actually dials may diverge
        // from the daemon-advertised one — docker port-forwarding, NAT,
        // and SSH tunnels all shift host:port from what daemon.json
        // reports. Liveness is no longer pre-probed here: the first
        // RPC (e.g. getStatus) surfaces connection failures with its
        // own timeout, and sharing that one code path avoids lying to
        // the user with a "probe ok, RPC fails" mismatch.
        const TransportInfo effective =
            ClientConnection::effectiveTransport(*chosen);

        LogosTransportConfig cfg;
        if (effective.protocol == "tcp")          cfg.protocol = LogosProtocol::Tcp;
        else if (effective.protocol == "tcp_ssl") cfg.protocol = LogosProtocol::TcpSsl;
        else                                      cfg.protocol = LogosProtocol::LocalSocket;
        cfg.host       = effective.host;
        cfg.port       = effective.port;
        cfg.caFile     = effective.caFile;
        cfg.verifyPeer = effective.verifyPeer;

        // Codec: use whatever the daemon advertised for this transport,
        // unless the caller pinned --client-codec. If the caller pinned a
        // codec that doesn't match the daemon, abort — talking JSON at
        // one end and CBOR at the other just produces a corrupted stream.
        const std::string advertisedCodec =
            chosen->codec.empty() ? std::string("json") : chosen->codec;
        const std::string requiredCodec =
            qEnvironmentVariable("LOGOSCORE_CLIENT_CODEC").toStdString();
        if (!requiredCodec.empty() && requiredCodec != advertisedCodec) {
            m_lastError = QString("Daemon's %1 transport uses codec '%2', but "
                                  "--client-codec requested '%3'.")
                              .arg(QString::fromStdString(chosen->protocol),
                                   QString::fromStdString(advertisedCodec),
                                   QString::fromStdString(requiredCodec));
            return false;
        }
        cfg.codec = (advertisedCodec == "cbor")
                  ? LogosWireCodec::Cbor
                  : LogosWireCodec::Json;
        LogosTransportConfigGlobal::setDefault(std::move(cfg));
    }

    // Create LogosAPI for this CLI client
    d->api = new LogosAPI("cli_client");

    // Get a client handle to the daemon's core_service module
    d->coreService = d->api->getClient("core_service");
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

    // If RPC failed, return basic info from connection file
    QJsonObject status;
    QJsonObject daemon;
    daemon["status"] = "running";
    daemon["pid"] = static_cast<qint64>(d->connInfo.pid);
    daemon["instance_id"] = QString::fromStdString(d->connInfo.instanceId);
    daemon["version"] = QCoreApplication::applicationVersion();
    if (!d->connInfo.startedAt.empty()) {
        QDateTime started = QDateTime::fromString(
            QString::fromStdString(d->connInfo.startedAt), Qt::ISODate);
        if (started.isValid()) {
            qint64 uptimeSecs = started.secsTo(QDateTime::currentDateTimeUtc());
            daemon["uptime_seconds"] = uptimeSecs;
        }
    }
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
