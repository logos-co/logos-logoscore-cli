#ifndef CORE_SERVICE_IMPL_H
#define CORE_SERVICE_IMPL_H

#include <logos_provider_object.h>
#include <functional>
#include <string>
#include <vector>
#include <logos_json.h>
#include <logos_result.h>

class CoreServiceImpl : public LogosProviderObject
{
public:
    std::function<void(const std::string& eventName, const std::string& data)> emitEvent;

    // Module lifecycle
    StdLogosResult loadModule(const std::string& name);
    StdLogosResult unloadModule(const std::string& name);
    StdLogosResult reloadModule(const std::string& name);

    // Queries
    LogosList listModules(const std::string& filter);
    LogosMap getStatus();
    LogosMap getModuleInfo(const std::string& name);
    LogosList getModuleStats();

    // Proxied call -- delegates to target module
    StdLogosResult callModuleMethod(const std::string& module,
                                    const std::string& method,
                                    const LogosList& args);

    // Event forwarding
    bool watchModuleEvents(const std::string& module,
                           const std::string& eventName);

    // Daemon lifecycle
    LogosMap shutdown();

    std::string name() const { return "core_service"; }
    std::string version() const { return "1.0.0"; }

    void onInit(LogosAPI* api);

    // LogosProviderObject Qt interface (delegates to std bridge)
    QVariant callMethod(const QString& methodName, const QVariantList& args) override;
    QJsonArray getMethods() override;
    QString providerName() const override;
    QString providerVersion() const override;
    void setEventListener(EventCallback callback) override;
    bool informModuleToken(const QString& moduleName, const QString& token) override;
    void init(void* apiInstance) override;

    // LogosProviderObject universal interface (Qt-free dispatch)
    nlohmann::json callMethodStd(const std::string& methodName, const nlohmann::json& args) override;
    std::vector<LogosMethodMetadata> getMethodsStd() override;
    void setEventListenerStd(UniversalEventCallback callback) override;

private:
    EventCallback m_eventCallback;
    LogosAPI* m_api = nullptr;

    // Helpers
    std::vector<std::string> getKnownModuleNames();
    std::vector<std::string> getLoadedModuleNames();
    // All known modules' info (name, path, loaded, dependencies, dependents,
    // metadata) as a JSON array, sourced from logos_core_get_modules_info.
    nlohmann::json getModulesInfo();
    // Version from a single module's embedded metadata, or "" if unknown or
    // the plugin declares none.
    std::string getModuleVersion(const std::string& name);
};

#endif // CORE_SERVICE_IMPL_H
