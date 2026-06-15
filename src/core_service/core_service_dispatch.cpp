// Universal dispatch for CoreServiceImpl.
// Implements the LogosProviderObject interface using the Qt-free std overrides
// and delegates to the universal-typed business methods.

#include "core_service_impl.h"
#include <logos_api.h>

// ---------------------------------------------------------------------------
// Helpers: extract the value payload from StdLogosResult
// ---------------------------------------------------------------------------

static nlohmann::json stdLogosResultToJson(const StdLogosResult& r)
{
    return r.value;
}

// ---------------------------------------------------------------------------
// LogosProviderObject overrides — trivial Qt delegates to std bridge
// ---------------------------------------------------------------------------

QString CoreServiceImpl::providerName() const
{
    return QString::fromStdString(name());
}

QString CoreServiceImpl::providerVersion() const
{
    return QString::fromStdString(version());
}

QVariant CoreServiceImpl::callMethod(const QString& methodName, const QVariantList& args)
{
    return callMethodStdBridge(methodName, args);
}

QJsonArray CoreServiceImpl::getMethods()
{
    return getMethodsStdBridge();
}

void CoreServiceImpl::setEventListener(EventCallback callback)
{
    m_eventCallback = callback;
    setEventListenerStdBridge(callback);
}

bool CoreServiceImpl::informModuleToken(const QString& /*moduleName*/, const QString& /*token*/)
{
    return false;
}

void CoreServiceImpl::init(void* apiInstance)
{
    onInit(static_cast<LogosAPI*>(apiInstance));
}

// ---------------------------------------------------------------------------
// Universal interface — Qt-free dispatch
// ---------------------------------------------------------------------------

nlohmann::json CoreServiceImpl::callMethodStd(const std::string& methodName,
                                               const nlohmann::json& args)
{
  // args[i].get<std::string>() throws type_error on a non-string arg; without
  // this catch the exception escapes the event loop and kills the daemon, so a
  // malformed RPC becomes a structured error instead of a crash.
  try {
    if (methodName == "loadModule" && args.size() >= 1)
        return stdLogosResultToJson(loadModule(args[0].get<std::string>()));

    if (methodName == "unloadModule" && args.size() >= 1)
        return stdLogosResultToJson(unloadModule(args[0].get<std::string>()));

    if (methodName == "reloadModule" && args.size() >= 1)
        return stdLogosResultToJson(reloadModule(args[0].get<std::string>()));

    if (methodName == "listModules") {
        std::string filter = args.size() >= 1 ? args[0].get<std::string>() : "all";
        return listModules(filter);
    }

    if (methodName == "getStatus")
        return getStatus();

    if (methodName == "getModuleInfo" && args.size() >= 1)
        return getModuleInfo(args[0].get<std::string>());

    if (methodName == "getModuleStats")
        return getModuleStats();

    if (methodName == "callModuleMethod" && args.size() >= 3)
        return stdLogosResultToJson(
            callModuleMethod(args[0].get<std::string>(),
                             args[1].get<std::string>(),
                             args[2]));

    if (methodName == "watchModuleEvents" && args.size() >= 2)
        return watchModuleEvents(args[0].get<std::string>(),
                                 args[1].get<std::string>());

    if (methodName == "shutdown")
        return shutdown();

    return nullptr;
  } catch (const std::exception& e) {
    // Malformed argument types (or any other dispatch-time failure) become a
    // structured error rather than an uncaught exception that kills the daemon.
    return nlohmann::json{
        {"status",  "error"},
        {"code",    "INVALID_ARGS"},
        {"message", std::string("invalid arguments: ") + e.what()},
    };
  }
}

void CoreServiceImpl::setEventListenerStd(UniversalEventCallback callback)
{
    emitEvent = [callback](const std::string& eventName, const std::string& data) {
        if (callback)
            callback(eventName, data);
    };
}

std::vector<LogosMethodMetadata> CoreServiceImpl::getMethodsStd()
{
    std::vector<LogosMethodMetadata> methods;

    auto mkParam = [](const std::string& name, const std::string& type) {
        nlohmann::json p;
        p["name"] = name;
        p["type"] = type;
        return p;
    };

    auto mkMethod = [&](const std::string& name, const nlohmann::json& params,
                        const std::string& ret) {
        LogosMethodMetadata m;
        m.name = name;
        m.returnType = ret;
        m.parameters = params;
        methods.push_back(std::move(m));
    };

    mkMethod("loadModule",
             nlohmann::json::array({mkParam("name", "string")}),
             "StdLogosResult");

    mkMethod("unloadModule",
             nlohmann::json::array({mkParam("name", "string")}),
             "StdLogosResult");

    mkMethod("reloadModule",
             nlohmann::json::array({mkParam("name", "string")}),
             "StdLogosResult");

    mkMethod("listModules",
             nlohmann::json::array({mkParam("filter", "string")}),
             "LogosList");

    mkMethod("getStatus",
             nlohmann::json::array(),
             "LogosMap");

    mkMethod("getModuleInfo",
             nlohmann::json::array({mkParam("name", "string")}),
             "LogosMap");

    mkMethod("getModuleStats",
             nlohmann::json::array(),
             "LogosList");

    mkMethod("callModuleMethod",
             nlohmann::json::array({mkParam("module", "string"),
                                    mkParam("method", "string"),
                                    mkParam("args", "LogosList")}),
             "StdLogosResult");

    mkMethod("watchModuleEvents",
             nlohmann::json::array({mkParam("module", "string"),
                                    mkParam("eventName", "string")}),
             "bool");

    mkMethod("shutdown",
             nlohmann::json::array(),
             "LogosMap");

    return methods;
}
