#ifndef CLIENT_H
#define CLIENT_H

#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QVariantList>
#include <functional>
#include <string>

// Abstract client interface for communicating with daemon's core_service.
// The real implementation (RpcClient) uses LogosAPIClient from logos-cpp-sdk.
// Tests can provide mock implementations.
class Client {
public:
    virtual ~Client() = default;

    virtual bool connect() = 0;
    virtual bool isConnected() const = 0;
    virtual std::string lastError() const = 0;

    // Module lifecycle
    virtual QJsonObject loadModule(const std::string& name) = 0;
    virtual QJsonObject unloadModule(const std::string& name) = 0;
    virtual QJsonObject reloadModule(const std::string& name) = 0;

    // Queries
    virtual QJsonArray listModules(const std::string& filter) = 0;
    virtual QJsonObject getStatus() = 0;
    virtual QJsonObject getModuleInfo(const std::string& name) = 0;
    virtual QJsonArray getModuleStats() = 0;

    // Proxied call
    virtual QJsonObject callModuleMethod(const std::string& module,
                                         const std::string& method,
                                         const QVariantList& args) = 0;

    // Daemon lifecycle
    virtual QJsonObject shutdown() = 0;

    // Event watching
    virtual bool watchModuleEvents(const std::string& module,
                                   const std::string& eventName,
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
    std::string lastError() const override;

    QJsonObject loadModule(const std::string& name) override;
    QJsonObject unloadModule(const std::string& name) override;
    QJsonObject reloadModule(const std::string& name) override;
    QJsonArray listModules(const std::string& filter) override;
    QJsonObject getStatus() override;
    QJsonObject getModuleInfo(const std::string& name) override;
    QJsonArray getModuleStats() override;
    QJsonObject callModuleMethod(const std::string& module,
                                 const std::string& method,
                                 const QVariantList& args) override;
    QJsonObject shutdown() override;
    bool watchModuleEvents(const std::string& module,
                           const std::string& eventName,
                           std::function<void(const QJsonObject&)> callback) override;

private:
    struct Impl;
    Impl* d;
    bool m_connected = false;
    std::string m_lastError;
};

#endif // CLIENT_H
