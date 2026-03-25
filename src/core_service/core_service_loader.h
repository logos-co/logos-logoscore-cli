#ifndef CORE_SERVICE_LOADER_H
#define CORE_SERVICE_LOADER_H

// CoreService plugin loader.
//
// This loader registers core_service as an in-process module during daemon
// startup, before entering the event loop. It uses the same LogosProviderPlugin
// interface as any dynamically loaded module.
//
// Build requirement: logos-cpp-sdk must provide:
//   - logos_provider_plugin.h (LogosProviderPlugin, PluginInterface)
//   - logos_provider_object.h (LogosProviderObject)

#ifdef LOGOS_SDK_AVAILABLE

#include "core_service_impl.h"
#include <QObject>
#include <logos_provider_plugin.h>

class CoreServiceLoader : public QObject, public PluginInterface, public LogosProviderPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID LogosProviderPlugin_iid FILE "metadata.json")
    Q_INTERFACES(PluginInterface LogosProviderPlugin)

public:
    QString name() const override { return "core_service"; }
    QString version() const override { return "1.0.0"; }
    LogosProviderObject* createProviderObject() override {
        return new CoreServiceImpl();
    }
};

#endif // LOGOS_SDK_AVAILABLE

#endif // CORE_SERVICE_LOADER_H
