#include "core_manager_api.h"

#include <QDebug>
#include <QStringList>

CoreManager::CoreManager(LogosAPI* api) : m_api(api), m_client(api->getClient("core_manager")), m_moduleName(QStringLiteral("core_manager")) {}

LogosObject* CoreManager::ensureReplica() {
    if (!m_eventReplica) {
        LogosObject* replica = m_client->requestObject(m_moduleName);
        if (!replica) {
            qWarning() << "CoreManager: failed to acquire remote object for events on" << m_moduleName;
            return nullptr;
        }
        m_eventReplica = replica;
    }
    return m_eventReplica;
}

bool CoreManager::on(const QString& eventName, RawEventCallback callback) {
    if (!callback) {
        qWarning() << "CoreManager: ignoring empty event callback for" << eventName;
        return false;
    }
    LogosObject* origin = ensureReplica();
    if (!origin) {
        return false;
    }
    m_client->onEvent(origin, eventName, callback);
    return true;
}

bool CoreManager::on(const QString& eventName, EventCallback callback) {
    if (!callback) {
        qWarning() << "CoreManager: ignoring empty event callback for" << eventName;
        return false;
    }
    return on(eventName, [callback](const QString&, const QVariantList& data) {
        callback(data);
    });
}

void CoreManager::setEventSource(LogosObject* source) {
    m_eventSource = source;
}

LogosObject* CoreManager::eventSource() const {
    return m_eventSource;
}

void CoreManager::trigger(const QString& eventName) {
    trigger(eventName, QVariantList{});
}

void CoreManager::trigger(const QString& eventName, const QVariantList& data) {
    if (!m_eventSource) {
        qWarning() << "CoreManager: no event source set for trigger" << eventName;
        return;
    }
    m_client->onEventResponse(m_eventSource, eventName, data);
}

void CoreManager::trigger(const QString& eventName, LogosObject* source, const QVariantList& data) {
    if (!source) {
        qWarning() << "CoreManager: cannot trigger" << eventName << "with null source";
        return;
    }
    m_client->onEventResponse(source, eventName, data);
}

void CoreManager::initialize(int argc, char* argv[]) {
    QStringList args;
    if (argv) {
        for (int i = 0; i < argc; ++i) {
            args << QString::fromUtf8(argv[i] ? argv[i] : "");
        }
    }
    m_client->invokeRemoteMethod("core_manager", "initialize", argc, args);
}

void CoreManager::setPluginsDirectory(const QString& directory) {
    m_client->invokeRemoteMethod("core_manager", "setPluginsDirectory", directory);
}

void CoreManager::start() {
    m_client->invokeRemoteMethod("core_manager", "start");
}

void CoreManager::cleanup() {
    m_client->invokeRemoteMethod("core_manager", "cleanup");
}

QStringList CoreManager::getLoadedPlugins() {
    QVariant _result = m_client->invokeRemoteMethod("core_manager", "getLoadedPlugins");
    return _result.toStringList();
}

QJsonArray CoreManager::getKnownPlugins() {
    QVariant _result = m_client->invokeRemoteMethod("core_manager", "getKnownPlugins");
    return qvariant_cast<QJsonArray>(_result);
}

QJsonArray CoreManager::getPluginMethods(const QString& pluginName) {
    QVariant _result = m_client->invokeRemoteMethod("core_manager", "getPluginMethods", pluginName);
    return qvariant_cast<QJsonArray>(_result);
}

void CoreManager::helloWorld() {
    m_client->invokeRemoteMethod("core_manager", "helloWorld");
}

bool CoreManager::loadPlugin(const QString& pluginName) {
    QVariant _result = m_client->invokeRemoteMethod("core_manager", "loadPlugin", pluginName);
    return _result.toBool();
}

bool CoreManager::unloadPlugin(const QString& pluginName) {
    QVariant _result = m_client->invokeRemoteMethod("core_manager", "unloadPlugin", pluginName);
    return _result.toBool();
}

QString CoreManager::processPlugin(const QString& filePath) {
    QVariant _result = m_client->invokeRemoteMethod("core_manager", "processPlugin", filePath);
    return _result.toString();
}
