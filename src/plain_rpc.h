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
    // Safe from several threads at once: core_service serves calls concurrently.
    bool subscribe(const std::string& eventName,
                   std::function<void(const std::string&,
                                      const nlohmann::json&)> callback);
    std::size_t subscriptionCount() const;
    // Cancels every subscription taken through this client.
    void unsubscribeAll();
    // The target's subscription edges (LP_SUB_*) and the reason for a loss;
    // replays the current state when installed. nullptr removes it.
    bool setSubscriptionStatusCallback(
        std::function<void(int state, const std::string& reason)> callback);
    // {"restart":"manual"} holds a lost subscription instead of re-arming it.
    bool setSubscriptionOptions(const nlohmann::json& options);

private:
    struct Subscription;
    using StatusCallback = std::function<void(int, const std::string&)>;
    static void onEvent(const char* name, const char* dataJson, void* userData);
    static void onStatus(int state, unsigned long long generation,
                         const char* reason, void* userData);

    lp_client* m_client = nullptr;
    mutable std::mutex m_subscriptionsMutex;
    std::vector<std::unique_ptr<Subscription>> m_subscriptions;
    // Every callback ever installed stays alive until lp_client_destroy, since
    // one being replaced may still be running.
    std::vector<std::unique_ptr<StatusCallback>> m_statusCallbacks;
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
