#ifndef LOGOSCTL_PLAIN_RPC_H
#define LOGOSCTL_PLAIN_RPC_H

#include <logos_protocol.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace logosctl {

inline constexpr const char* kPlainLocalTransport =
    R"({"protocol":"qt_remote_plain"})";

struct PlainRpcError {
    std::string code;
    std::string message;
    std::string origin;

    bool ok() const { return code.empty(); }
};

class PlainRpcClient {
public:
    PlainRpcClient(std::string target, std::string origin,
                   std::string targetTransport = kPlainLocalTransport,
                   std::string capabilityTransport = kPlainLocalTransport);
    ~PlainRpcClient();

    PlainRpcClient(const PlainRpcClient&) = delete;
    PlainRpcClient& operator=(const PlainRpcClient&) = delete;

    bool valid() const { return m_client != nullptr; }
    nlohmann::json invoke(const std::string& method,
                          const nlohmann::json& args = nlohmann::json::array(),
                          int timeoutMs = 0,
                          PlainRpcError* error = nullptr);
    nlohmann::json methods();
    bool subscribe(const std::string& eventName,
                   std::function<void(const std::string&,
                                      const nlohmann::json&)> callback);

private:
    struct Subscription;
    static void onEvent(const char* name, const char* dataJson, void* userData);

    lp_client* m_client = nullptr;
    std::vector<std::unique_ptr<Subscription>> m_subscriptions;
};

class PlainRpcContext {
public:
    explicit PlainRpcContext(std::string origin,
                             std::string capabilityTransport = kPlainLocalTransport);
    ~PlainRpcContext() = default;

    PlainRpcClient* client(const std::string& target,
                           const std::string& targetTransport = kPlainLocalTransport);

private:
    std::string m_origin;
    std::string m_capabilityTransport;
    std::mutex m_mutex;
    std::map<std::pair<std::string, std::string>,
             std::unique_ptr<PlainRpcClient>> m_clients;
};

} // namespace logosctl

#endif
