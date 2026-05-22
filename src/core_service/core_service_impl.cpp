#include "core_service_impl.h"
#include "logos_core.h"
#include <logos_api.h>
#include <logos_api_client.h>

#include <QCoreApplication>
#include <QTimer>
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
            // Use the nlohmann::json overload — no QJson types needed here
            LogosAPIClient* moduleClient = m_api->getClient(QString::fromStdString(name));
            if (moduleClient) {
                nlohmann::json methods = moduleClient->invokeRemoteMethod(
                    name, "getPluginMethods", nlohmann::json::array());
                if (methods.is_array())
                    info["methods"] = methods;
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
// Proxied call — uses the nlohmann::json SDK overload; no QJson needed
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

    LogosAPIClient* moduleClient = m_api->getClient(QString::fromStdString(module));
    if (!moduleClient) {
        result["status"] = "error";
        result["code"] = "MODULE_NOT_LOADED";
        result["message"] = "Module '" + module + "' is not loaded. Load it with: logoscore load-module " + module;
        return {false, result, "Module '" + module + "' is not loaded."};
    }

    // The nlohmann::json overload handles QVariant<->json conversion internally.
    nlohmann::json ret = moduleClient->invokeRemoteMethod(module, method, args);

    if (ret.is_null()) {
        result["status"] = "error";
        result["code"] = "METHOD_FAILED";
        result["message"] = "Call to " + module + "." + method + " failed.";
        return {false, result, "Call to " + module + "." + method + " failed."};
    }

    result["status"] = "ok";
    result["module"] = module;
    result["method"] = method;
    result["result"] = ret;

    return {true, result};
}

// ---------------------------------------------------------------------------
// Event forwarding — uses the nlohmann::json onEvent overload
// ---------------------------------------------------------------------------

bool CoreServiceImpl::watchModuleEvents(const std::string& module,
                                        const std::string& eventName)
{
    if (!m_api)
        return false;

    LogosAPIClient* moduleClient = m_api->getClient(QString::fromStdString(module));
    if (!moduleClient)
        return false;

    LogosObject* obj = moduleClient->requestObject(QString::fromStdString(module));
    if (!obj)
        return false;

    moduleClient->onEvent(obj, eventName,
        [this, module](const std::string& event, const nlohmann::json& data) {
            nlohmann::json forwardData = nlohmann::json::array();
            forwardData.push_back(module);
            forwardData.push_back(event);
            if (data.is_array()) {
                for (const auto& item : data)
                    forwardData.push_back(item);
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
