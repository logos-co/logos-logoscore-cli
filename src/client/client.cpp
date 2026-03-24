#include "client.h"
#include "../config.h"
#include "../daemon/connection_file.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_instance.h>
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
    // Read connection file
    d->connInfo = ConnectionFile::read();
    if (!d->connInfo.valid) {
        m_lastError = "No running logoscore daemon. Start one with: logoscore -D";
        return false;
    }

    d->instanceId = d->connInfo.instanceId;

    // Resolve token
    d->token = Config::getToken();
    if (d->token.isEmpty())
        d->token = d->connInfo.token;

    if (d->token.isEmpty()) {
        m_lastError = "No authentication token found.";
        return false;
    }

    // Set LOGOS_INSTANCE_ID so LogosInstance::id() returns the correct registry URL
    qputenv("LOGOS_INSTANCE_ID", d->instanceId.toUtf8());

    // Store token in TokenManager so the SDK can authenticate
    TokenManager::instance().saveToken("cli_client", d->token);

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
    daemon["pid"] = d->connInfo.pid;
    daemon["instance_id"] = d->connInfo.instanceId;
    daemon["version"] = QCoreApplication::applicationVersion();
    if (d->connInfo.startedAt.isValid()) {
        qint64 uptimeSecs = d->connInfo.startedAt.secsTo(QDateTime::currentDateTimeUtc());
        daemon["uptime_seconds"] = uptimeSecs;
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
