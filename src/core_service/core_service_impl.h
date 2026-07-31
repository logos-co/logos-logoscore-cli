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
    // `withDependents` cascades the unload to every module that depends on
    // `name`, leaves-first. It defaults to true at the CLI layer: unloading a
    // module while its dependents keep running leaves them talking to a dead
    // provider, so the cascade is the safe default and opting out is explicit.
    StdLogosResult unloadModule(const std::string& name, bool withDependents);
    StdLogosResult reloadModule(const std::string& name);

    // Re-scan the module directories so packages installed since boot become
    // discoverable without restarting the daemon. This is what lets
    // `install` be followed by `load` in the same session.
    LogosMap refreshModules();

    // Package operations. Split into plan/apply so the client can show what
    // would change (and prompt) before anything is written — `--dry-run`
    // stops after the plan. See package_ops.h for why the work happens here
    // rather than in the client.
    LogosMap planPackageOperation(const std::string& op,
                                  const LogosList& names,
                                  const LogosMap& opts);
    LogosMap applyPackageOperation(const std::string& op,
                                   const LogosList& names,
                                   const LogosMap& opts);

    // Fetch a .lgx without installing it. Daemon-side rather than a direct
    // proxy to package_downloader because the downloader has no destination
    // parameter -- it drops the file in $TMPDIR, and the move to the requested
    // directory has to happen on the host that holds it.
    LogosMap downloadPackage(const std::string& name, const LogosMap& opts);

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
