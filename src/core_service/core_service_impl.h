#ifndef CORE_SERVICE_IMPL_H
#define CORE_SERVICE_IMPL_H

#include <logos_provider_object.h>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QVariantList>
#include <QString>

class CoreServiceImpl : public LogosProviderBase
{
    LOGOS_PROVIDER(CoreServiceImpl, "core_service", "1.0.0")

public:
    // Module lifecycle
    LOGOS_METHOD QVariant loadModule(const QString& name);
    LOGOS_METHOD QVariant unloadModule(const QString& name);
    LOGOS_METHOD QVariant reloadModule(const QString& name);

    // Queries
    LOGOS_METHOD QJsonArray listModules(const QString& filter);
    LOGOS_METHOD QJsonObject getStatus();
    LOGOS_METHOD QJsonObject getModuleInfo(const QString& name);
    LOGOS_METHOD QJsonArray getModuleStats();

    // Proxied call -- delegates to target module
    LOGOS_METHOD QVariant callModuleMethod(const QString& module,
                                          const QString& method,
                                          const QVariantList& args);

    // Event forwarding
    LOGOS_METHOD bool watchModuleEvents(const QString& module,
                                       const QString& eventName);

    // Daemon lifecycle
    LOGOS_METHOD QJsonObject shutdown();

protected:
    void onInit(LogosAPI* api) override;

private:
    LogosAPI* m_api = nullptr;

    // Helpers
    QStringList getKnownPluginNames();
    QStringList getLoadedPluginNames();
};

#endif // CORE_SERVICE_IMPL_H
