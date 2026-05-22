#ifndef CLIENT_H
#define CLIENT_H

#include <logos_json.h>
#include <functional>
#include <string>
#include <vector>

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
    virtual LogosMap loadModule(const std::string& name) = 0;
    virtual LogosMap unloadModule(const std::string& name) = 0;
    virtual LogosMap reloadModule(const std::string& name) = 0;

    // Queries
    virtual LogosList listModules(const std::string& filter) = 0;
    virtual LogosMap getStatus() = 0;
    virtual LogosMap getModuleInfo(const std::string& name) = 0;
    virtual LogosList getModuleStats() = 0;

    // Proxied call — args is a json array of scalar/object arguments
    virtual LogosMap callModuleMethod(const std::string& module,
                                      const std::string& method,
                                      const LogosList& args) = 0;

    // Daemon lifecycle
    virtual LogosMap shutdown() = 0;

    // Event watching
    virtual bool watchModuleEvents(const std::string& module,
                                   const std::string& eventName,
                                   std::function<void(const LogosMap&)> callback) = 0;
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

    LogosMap loadModule(const std::string& name) override;
    LogosMap unloadModule(const std::string& name) override;
    LogosMap reloadModule(const std::string& name) override;
    LogosList listModules(const std::string& filter) override;
    LogosMap getStatus() override;
    LogosMap getModuleInfo(const std::string& name) override;
    LogosList getModuleStats() override;
    LogosMap callModuleMethod(const std::string& module,
                              const std::string& method,
                              const LogosList& args) override;
    LogosMap shutdown() override;
    bool watchModuleEvents(const std::string& module,
                           const std::string& eventName,
                           std::function<void(const LogosMap&)> callback) override;

private:
    struct Impl;
    Impl* d;
    bool m_connected = false;
    std::string m_lastError;
};

#endif // CLIENT_H
