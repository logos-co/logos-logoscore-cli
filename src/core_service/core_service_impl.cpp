#include "core_service_impl.h"
#include "logos_core.h"
#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_types.h>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMetaType>
#include <QTimer>
#include <QString>
#include <QVariant>
#include <algorithm>
#include <cstdlib>
#include <unistd.h>

void CoreServiceImpl::onInit(LogosAPI* api)
{
    m_api = api;
}

// ---------------------------------------------------------------------------
// Helpers to convert C API char** to std::vector<std::string>
// ---------------------------------------------------------------------------

std::vector<std::string> CoreServiceImpl::getKnownModuleNames()
{
    std::vector<std::string> result;
    char** modules = logos_core_get_known_modules();
    if (modules) {
        for (int i = 0; modules[i] != nullptr; ++i) {
            result.emplace_back(modules[i]);
            free(modules[i]);
        }
        free(modules);
    }
    return result;
}

std::vector<std::string> CoreServiceImpl::getLoadedModuleNames()
{
    std::vector<std::string> result;
    char** modules = logos_core_get_loaded_modules();
    if (modules) {
        for (int i = 0; modules[i] != nullptr; ++i) {
            result.emplace_back(modules[i]);
            free(modules[i]);
        }
        free(modules);
    }
    return result;
}

static bool containsName(const std::vector<std::string>& v, const std::string& name)
{
    return std::find(v.begin(), v.end(), name) != v.end();
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

StdLogosResult CoreServiceImpl::loadModule(const std::string& name)
{
    bool ok = logos_core_load_module(name.c_str(), true);
    if (!ok) {
        LogosMap errResult;
        errResult["status"] = "error";
        errResult["code"] = "MODULE_LOAD_FAILED";
        errResult["message"] = "Failed to load module '" + name + "'.";

        auto known = getKnownModuleNames();
        LogosList names = LogosList::array();
        for (const auto& n : known)
            names.push_back(n);
        errResult["known_modules"] = names;
        return {false, errResult, "Failed to load module '" + name + "'."};
    }

    LogosMap result;
    result["status"] = "ok";
    result["module"] = name;
    result["dependencies_loaded"] = LogosList::array();
    return {true, result};
}

StdLogosResult CoreServiceImpl::unloadModule(const std::string& name)
{
    logos_core_unload_module(name.c_str(), false);

    auto loaded = getLoadedModuleNames();
    if (containsName(loaded, name)) {
        LogosMap errResult;
        errResult["status"] = "error";
        errResult["code"] = "MODULE_NOT_LOADED";
        errResult["message"] = "Module '" + name + "' is not loaded.";
        return {false, errResult, "Module '" + name + "' is not loaded."};
    }

    LogosMap result;
    result["status"] = "ok";
    result["module"] = name;
    return {true, result};
}

StdLogosResult CoreServiceImpl::reloadModule(const std::string& name)
{
    LogosMap result;
    result["action"] = "reload";
    result["module"] = name;

    auto loaded = getLoadedModuleNames();
    std::string previousStatus = containsName(loaded, name) ? "loaded" : "not_loaded";
    result["previous_status"] = previousStatus;

    if (containsName(loaded, name)) {
        logos_core_unload_module(name.c_str(), false);
    }

    bool ok = logos_core_load_module(name.c_str(), true);
    if (!ok) {
        result["status"] = "error";
        result["error"] = "module failed to start";
        return {false, result, "module failed to start"};
    }

    result["status"] = "loaded";
    return {true, result};
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

LogosList CoreServiceImpl::listModules(const std::string& filter)
{
    LogosList modules = LogosList::array();

    auto known = getKnownModuleNames();
    auto loaded = getLoadedModuleNames();

    for (const auto& name : known) {
        LogosMap mod;
        mod["name"] = name;

        if (containsName(loaded, name)) {
            mod["status"] = "loaded";
        } else {
            mod["status"] = "not_loaded";
        }

        if (filter == "loaded" && !containsName(loaded, name))
            continue;

        modules.push_back(mod);
    }

    return modules;
}

LogosMap CoreServiceImpl::getStatus()
{
    LogosMap status;

    LogosMap daemon;
    daemon["status"] = "running";
    daemon["pid"] = static_cast<int64_t>(getpid());
    daemon["version"] = version();
    status["daemon"] = daemon;

    LogosList modules = listModules("all");
    status["modules"] = modules;

    int loadedCount = 0, crashed = 0, notLoaded = 0;
    for (const auto& v : modules) {
        std::string s = v.value("status", "");
        if (s == "loaded") loadedCount++;
        else if (s == "crashed") crashed++;
        else notLoaded++;
    }
    LogosMap summary;
    summary["loaded"] = loadedCount;
    summary["crashed"] = crashed;
    summary["not_loaded"] = notLoaded;
    status["modules_summary"] = summary;

    return status;
}

LogosMap CoreServiceImpl::getModuleInfo(const std::string& name)
{
    LogosMap info;

    auto known = getKnownModuleNames();
    if (!containsName(known, name)) {
        info["status"] = "error";
        info["code"] = "MODULE_NOT_FOUND";
        info["message"] = "Module '" + name + "' not found.";
        return info;
    }

    info["name"] = name;

    auto loaded = getLoadedModuleNames();
    if (containsName(loaded, name)) {
        info["status"] = "loaded";

        if (m_api) {
            QString qName = QString::fromStdString(name);
            LogosAPIClient* moduleClient = m_api->getClient(qName);
            if (moduleClient) {
                QVariant ret = moduleClient->invokeRemoteMethod(qName, "getPluginMethods");
                if (ret.canConvert<QJsonArray>()) {
                    QJsonArray methods = qvariant_cast<QJsonArray>(ret);
                    QJsonDocument doc(methods);
                    info["methods"] = nlohmann::json::parse(
                        doc.toJson(QJsonDocument::Compact).toStdString());
                }
            }
        }
    } else {
        info["status"] = "not_loaded";
    }

    return info;
}

LogosList CoreServiceImpl::getModuleStats()
{
    LogosList stats = LogosList::array();
    char* json = logos_core_get_module_stats();
    if (json) {
        try {
            stats = nlohmann::json::parse(json);
        } catch (...) {}
        free(json);
    }
    return stats;
}

// ---------------------------------------------------------------------------
// Proxied call
// ---------------------------------------------------------------------------

StdLogosResult CoreServiceImpl::callModuleMethod(const std::string& module,
                                                 const std::string& method,
                                                 const LogosList& args)
{
    LogosMap result;

    if (!m_api) {
        result["status"]  = "error";
        result["code"]    = "INTERNAL_ERROR";
        result["message"] = "core_service not initialized.";
        return {false, result, "core_service not initialized."};
    }

    QString qModule = QString::fromStdString(module);
    QString qMethod = QString::fromStdString(method);

    LogosAPIClient* moduleClient = m_api->getClient(qModule);
    if (!moduleClient) {
        result["status"] = "error";
        result["code"] = "MODULE_NOT_LOADED";
        result["message"] = "Module '" + module + "' is not loaded. Load it with: logoscore load-module " + module;
        return {false, result, "Module '" + module + "' is not loaded."};
    }

    // Convert LogosList args to QVariantList for the Qt SDK call
    QVariantList qArgs;
    for (const auto& arg : args) {
        if (arg.is_string())
            qArgs.append(QString::fromStdString(arg.get<std::string>()));
        else if (arg.is_number_integer())
            qArgs.append(static_cast<qlonglong>(arg.get<int64_t>()));
        else if (arg.is_number_float())
            qArgs.append(arg.get<double>());
        else if (arg.is_boolean())
            qArgs.append(arg.get<bool>());
        else if (arg.is_array()) {
            QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(arg.dump()));
            qArgs.append(QVariant::fromValue(doc.array()));
        } else if (arg.is_object()) {
            QJsonDocument doc = QJsonDocument::fromJson(
                QByteArray::fromStdString(arg.dump()));
            qArgs.append(QVariant::fromValue(doc.object()));
        } else {
            qArgs.append(QVariant());
        }
    }

    QVariant ret = moduleClient->invokeRemoteMethod(qModule, qMethod, qArgs);

    if (!ret.isValid()) {
        result["status"] = "error";
        result["code"] = "METHOD_FAILED";
        result["message"] = "Call to " + module + "." + method + " failed.";
        return {false, result, "Call to " + module + "." + method + " failed."};
    }

    result["status"] = "ok";
    result["module"] = module;
    result["method"] = method;

    const int logosResultId = QMetaType::fromName("LogosResult").id();
    if (logosResultId != QMetaType::UnknownType
            && ret.userType() == logosResultId) {
        const LogosResult lr = ret.value<LogosResult>();
        LogosMap obj;
        obj["success"] = lr.success;
        QJsonValue valJson = QJsonValue::fromVariant(lr.value);
        QJsonValue errJson = QJsonValue::fromVariant(lr.error);
        QJsonDocument valDoc;
        if (valJson.isObject()) valDoc = QJsonDocument(valJson.toObject());
        else if (valJson.isArray()) valDoc = QJsonDocument(valJson.toArray());
        if (!valDoc.isNull())
            obj["value"] = nlohmann::json::parse(valDoc.toJson(QJsonDocument::Compact).toStdString());
        else if (valJson.isString())
            obj["value"] = valJson.toString().toStdString();
        else if (valJson.isBool())
            obj["value"] = valJson.toBool();
        else if (valJson.isDouble())
            obj["value"] = valJson.toDouble();
        else
            obj["value"] = nullptr;
        if (errJson.isString())
            obj["error"] = errJson.toString().toStdString();
        else
            obj["error"] = nullptr;
        result["result"] = obj;
    } else if (ret.canConvert<QJsonObject>()) {
        QJsonDocument doc(ret.toJsonObject());
        result["result"] = nlohmann::json::parse(
            doc.toJson(QJsonDocument::Compact).toStdString());
    } else if (ret.canConvert<QJsonArray>()) {
        QJsonDocument doc(ret.toJsonArray());
        result["result"] = nlohmann::json::parse(
            doc.toJson(QJsonDocument::Compact).toStdString());
    } else {
        QJsonValue jv = QJsonValue::fromVariant(ret);
        if (jv.isString())
            result["result"] = jv.toString().toStdString();
        else if (jv.isBool())
            result["result"] = jv.toBool();
        else if (jv.isDouble())
            result["result"] = jv.toDouble();
        else
            result["result"] = nullptr;
    }

    return {true, result};
}

// ---------------------------------------------------------------------------
// Event forwarding
// ---------------------------------------------------------------------------

bool CoreServiceImpl::watchModuleEvents(const std::string& module,
                                        const std::string& eventName)
{
    if (!m_api)
        return false;

    QString qModule = QString::fromStdString(module);
    QString qEventName = QString::fromStdString(eventName);

    LogosAPIClient* moduleClient = m_api->getClient(qModule);
    if (!moduleClient)
        return false;

    LogosObject* obj = moduleClient->requestObject(qModule);
    if (!obj)
        return false;

    moduleClient->onEvent(obj, qEventName,
        [this, module](const QString& event, const QVariantList& data) {
            nlohmann::json forwardData = nlohmann::json::array();
            forwardData.push_back(module);
            forwardData.push_back(event.toStdString());
            for (const QVariant& d : data) {
                QJsonValue jv = QJsonValue::fromVariant(d);
                if (jv.isString())
                    forwardData.push_back(jv.toString().toStdString());
                else if (jv.isBool())
                    forwardData.push_back(jv.toBool());
                else if (jv.isDouble())
                    forwardData.push_back(jv.toDouble());
                else if (jv.isObject() || jv.isArray()) {
                    QJsonDocument doc;
                    if (jv.isObject()) doc = QJsonDocument(jv.toObject());
                    else doc = QJsonDocument(jv.toArray());
                    forwardData.push_back(nlohmann::json::parse(
                        doc.toJson(QJsonDocument::Compact).toStdString()));
                } else {
                    forwardData.push_back(nullptr);
                }
            }
            if (emitEvent)
                emitEvent("module_event", forwardData.dump());
        });

    return true;
}

// ---------------------------------------------------------------------------
// Daemon lifecycle
// ---------------------------------------------------------------------------

LogosMap CoreServiceImpl::shutdown()
{
    LogosMap result;
    result["status"] = "ok";
    result["message"] = "Daemon shutting down.";

    QTimer::singleShot(200, QCoreApplication::instance(), &QCoreApplication::quit);

    return result;
}
