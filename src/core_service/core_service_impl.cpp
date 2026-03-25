#include "core_service_impl.h"
#include "logos_core.h"
#include <logos_api.h>
#include <logos_api_client.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QDebug>
#include <QTimer>
#include <cstdlib>

void CoreServiceImpl::onInit(LogosAPI* api)
{
    m_api = api;
}

// ---------------------------------------------------------------------------
// Helpers to convert C API char** to QStringList
// ---------------------------------------------------------------------------

QStringList CoreServiceImpl::getKnownPluginNames()
{
    QStringList result;
    char** plugins = logos_core_get_known_plugins();
    if (plugins) {
        for (int i = 0; plugins[i] != nullptr; ++i) {
            result.append(QString::fromUtf8(plugins[i]));
            free(plugins[i]);
        }
        free(plugins);
    }
    return result;
}

QStringList CoreServiceImpl::getLoadedPluginNames()
{
    QStringList result;
    char** plugins = logos_core_get_loaded_plugins();
    if (plugins) {
        for (int i = 0; plugins[i] != nullptr; ++i) {
            result.append(QString::fromUtf8(plugins[i]));
            free(plugins[i]);
        }
        free(plugins);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

QVariant CoreServiceImpl::loadModule(const QString& name)
{
    QJsonObject result;

    bool ok = logos_core_load_plugin_with_dependencies(name.toUtf8().constData());
    if (!ok) {
        result["status"] = "error";
        result["code"] = "MODULE_LOAD_FAILED";
        result["message"] = QString("Failed to load module '%1'.").arg(name);

        // Include known modules for context
        QStringList known = getKnownPluginNames();
        QJsonArray names;
        for (const QString& n : known)
            names.append(n);
        result["known_modules"] = names;
        return QVariant::fromValue(result);
    }

    result["status"] = "ok";
    result["module"] = name;
    result["dependencies_loaded"] = QJsonArray();
    return QVariant::fromValue(result);
}

QVariant CoreServiceImpl::unloadModule(const QString& name)
{
    QJsonObject result;

    logos_core_unload_plugin(name.toUtf8().constData());

    // Check if it was actually unloaded
    QStringList loaded = getLoadedPluginNames();
    if (loaded.contains(name)) {
        result["status"] = "error";
        result["code"] = "MODULE_NOT_LOADED";
        result["message"] = QString("Module '%1' is not loaded.").arg(name);
        return QVariant::fromValue(result);
    }

    result["status"] = "ok";
    result["module"] = name;
    return QVariant::fromValue(result);
}

QVariant CoreServiceImpl::reloadModule(const QString& name)
{
    QJsonObject result;
    result["action"] = "reload";
    result["module"] = name;

    QStringList loaded = getLoadedPluginNames();
    QString previousStatus = loaded.contains(name) ? "loaded" : "not_loaded";
    result["previous_status"] = previousStatus;

    // Unload if loaded
    if (loaded.contains(name)) {
        logos_core_unload_plugin(name.toUtf8().constData());
    }

    // Load
    bool ok = logos_core_load_plugin_with_dependencies(name.toUtf8().constData());
    if (!ok) {
        result["status"] = "error";
        result["error"] = "module failed to start";
        return QVariant::fromValue(result);
    }

    result["status"] = "loaded";
    return QVariant::fromValue(result);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

QJsonArray CoreServiceImpl::listModules(const QString& filter)
{
    QJsonArray modules;

    QStringList known = getKnownPluginNames();
    QStringList loaded = getLoadedPluginNames();

    for (const QString& name : known) {
        QJsonObject mod;
        mod["name"] = name;

        if (loaded.contains(name)) {
            mod["status"] = "loaded";
        } else {
            mod["status"] = "not_loaded";
        }

        if (filter == "loaded" && !loaded.contains(name))
            continue;

        modules.append(mod);
    }

    return modules;
}

QJsonObject CoreServiceImpl::getStatus()
{
    QJsonObject status;

    QJsonObject daemon;
    daemon["status"] = "running";
    daemon["pid"] = QCoreApplication::applicationPid();
    daemon["version"] = QCoreApplication::applicationVersion();
    status["daemon"] = daemon;

    QJsonArray modules = listModules("all");
    status["modules"] = modules;

    int loadedCount = 0, crashed = 0, notLoaded = 0;
    for (const QJsonValue& v : modules) {
        QString s = v.toObject().value("status").toString();
        if (s == "loaded") loadedCount++;
        else if (s == "crashed") crashed++;
        else notLoaded++;
    }
    QJsonObject summary;
    summary["loaded"] = loadedCount;
    summary["crashed"] = crashed;
    summary["not_loaded"] = notLoaded;
    status["modules_summary"] = summary;

    return status;
}

QJsonObject CoreServiceImpl::getModuleInfo(const QString& name)
{
    QJsonObject info;

    QStringList known = getKnownPluginNames();
    if (!known.contains(name)) {
        info["status"] = "error";
        info["code"] = "MODULE_NOT_FOUND";
        info["message"] = QString("Module '%1' not found.").arg(name);
        return info;
    }

    info["name"] = name;

    QStringList loaded = getLoadedPluginNames();
    if (loaded.contains(name)) {
        info["status"] = "loaded";

        // Query the module's published methods via SDK
        if (m_api) {
            LogosAPIClient* moduleClient = m_api->getClient(name);
            if (moduleClient) {
                QVariant ret = moduleClient->invokeRemoteMethod(name, "getPluginMethods");
                if (ret.canConvert<QJsonArray>()) {
                    info["methods"] = qvariant_cast<QJsonArray>(ret);
                }
            }
        }
    } else {
        info["status"] = "not_loaded";
    }

    return info;
}

QJsonArray CoreServiceImpl::getModuleStats()
{
    QJsonArray stats;
    char* json = logos_core_get_module_stats();
    if (json) {
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json));
        if (doc.isArray())
            stats = doc.array();
        free(json);
    }
    return stats;
}

// ---------------------------------------------------------------------------
// Proxied call
// ---------------------------------------------------------------------------

QVariant CoreServiceImpl::callModuleMethod(const QString& module,
                                           const QString& method,
                                           const QVariantList& args)
{
    QJsonObject result;

    if (!m_api) {
        result["status"] = "error";
        result["code"] = "INTERNAL_ERROR";
        result["message"] = "core_service not initialized.";
        return QVariant::fromValue(result);
    }

    LogosAPIClient* moduleClient = m_api->getClient(module);
    if (!moduleClient) {
        result["status"] = "error";
        result["code"] = "MODULE_NOT_LOADED";
        result["message"] = QString("Module '%1' is not loaded. Load it with: logoscore load-module %1").arg(module);
        return QVariant::fromValue(result);
    }

    QVariant ret = moduleClient->invokeRemoteMethod(module, method, args);

    if (!ret.isValid()) {
        result["status"] = "error";
        result["code"] = "METHOD_FAILED";
        result["message"] = QString("Call to %1.%2 failed.").arg(module, method);
        return QVariant::fromValue(result);
    }

    result["status"] = "ok";
    result["module"] = module;
    result["method"] = method;

    if (ret.canConvert<QJsonObject>()) {
        result["result"] = ret.toJsonObject();
    } else if (ret.canConvert<QJsonArray>()) {
        result["result"] = ret.toJsonArray();
    } else {
        result["result"] = QJsonValue::fromVariant(ret);
    }

    return QVariant::fromValue(result);
}

// ---------------------------------------------------------------------------
// Event forwarding
// ---------------------------------------------------------------------------

bool CoreServiceImpl::watchModuleEvents(const QString& module,
                                        const QString& eventName)
{
    if (!m_api)
        return false;

    LogosAPIClient* moduleClient = m_api->getClient(module);
    if (!moduleClient)
        return false;

    LogosObject* obj = moduleClient->requestObject(module);
    if (!obj)
        return false;

    moduleClient->onEvent(obj, eventName,
        [this, module](const QString& event, const QVariantList& data) {
            // Forward the event through core_service's event system
            QVariantList forwardData;
            forwardData.append(module);
            forwardData.append(event);
            for (const QVariant& d : data)
                forwardData.append(d);
            emitEvent("module_event", forwardData);
        });

    return true;
}

// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

QJsonObject CoreServiceImpl::shutdown()
{
    QJsonObject result;
    result["status"] = "ok";
    result["message"] = "Daemon shutting down.";

    // Give the RPC layer time to send the response before we quit
    QTimer::singleShot(200, QCoreApplication::instance(), &QCoreApplication::quit);

    return result;
}
