#ifndef CLIENT_H
#define CLIENT_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QVariantList>
#include <functional>

// Abstract client interface for communicating with daemon's core_service.
// The real implementation (RpcClient) uses LogosAPIClient from logos-cpp-sdk.
// Tests can provide mock implementations.
class Client {
public:
    virtual ~Client() = default;

    virtual bool connect() = 0;
    virtual bool isConnected() const = 0;
    virtual QString lastError() const = 0;

    // Module lifecycle
    virtual QJsonObject loadModule(const QString& name) = 0;
    virtual QJsonObject unloadModule(const QString& name) = 0;
    virtual QJsonObject reloadModule(const QString& name) = 0;

    // Queries
    virtual QJsonArray listModules(const QString& filter) = 0;
    virtual QJsonObject getStatus() = 0;
    virtual QJsonObject getModuleInfo(const QString& name) = 0;
    virtual QJsonArray getModuleStats() = 0;

    // Proxied call
    virtual QJsonObject callModuleMethod(const QString& module,
                                         const QString& method,
                                         const QVariantList& args) = 0;

    // Daemon lifecycle
    virtual QJsonObject shutdown() = 0;

    // Event watching
    virtual bool watchModuleEvents(const QString& module,
                                   const QString& eventName,
                                   std::function<void(const QJsonObject&)> callback) = 0;
};

// Real RPC client implementation that connects to the daemon.
// Depends on logos-cpp-sdk (LogosAPIClient).
class RpcClient : public Client {
public:
    RpcClient();
    ~RpcClient() override;

    bool connect() override;
    bool isConnected() const override;
    QString lastError() const override;

    QJsonObject loadModule(const QString& name) override;
    QJsonObject unloadModule(const QString& name) override;
    QJsonObject reloadModule(const QString& name) override;
    QJsonArray listModules(const QString& filter) override;
    QJsonObject getStatus() override;
    QJsonObject getModuleInfo(const QString& name) override;
    QJsonArray getModuleStats() override;
    QJsonObject callModuleMethod(const QString& module,
                                 const QString& method,
                                 const QVariantList& args) override;
    QJsonObject shutdown() override;
    bool watchModuleEvents(const QString& module,
                           const QString& eventName,
                           std::function<void(const QJsonObject&)> callback) override;

private:
    struct Impl;
    Impl* d;
    bool m_connected = false;
    QString m_lastError;
};

#endif // CLIENT_H
