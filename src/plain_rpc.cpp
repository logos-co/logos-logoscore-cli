#include "plain_rpc.h"

#include <cstdlib>
#include <utility>

namespace logosctl {

namespace {

nlohmann::json parseJson(const char* text)
{
    if (!text) return nullptr;
    nlohmann::json value = nlohmann::json::parse(text, nullptr, false);
    return value.is_discarded() ? nlohmann::json(nullptr) : value;
}

PlainRpcError parseError(const char* text)
{
    PlainRpcError result;
    const nlohmann::json value = parseJson(text);
    if (value.is_object()) {
        result.code = value.value("code", std::string{"transport_error"});
        result.message = value.value("message", std::string{"RPC failed"});
        result.origin = value.value("origin", std::string{});
    } else {
        result.code = "transport_error";
        result.message = text ? text : "RPC failed";
    }
    return result;
}

} // namespace

struct PlainRpcClient::Subscription {
    lp_subscription* handle = nullptr;
    std::function<void(const std::string&, const nlohmann::json&)> callback;

    ~Subscription()
    {
        lp_unsubscribe(handle);
    }
};

PlainRpcClient::PlainRpcClient(std::string target, std::string origin,
                               std::string targetTransport,
                               std::string capabilityTransport)
{
    m_client = lp_client_create(target.c_str(), origin.c_str(),
                                targetTransport.c_str(),
                                capabilityTransport.c_str());
}

bool PlainRpcClient::useSession(const std::string& chainPem, const std::string& keyPem,
                                const nlohmann::json& dial, const nlohmann::json& hello)
{
    if (!m_client) return false;
    m_session = std::make_unique<Session>(Session{dial.dump(), hello.dump()});
    return lp_client_set_tls_credential(m_client, chainPem.c_str(), keyPem.c_str()) == LP_OK
        && lp_client_set_session_hook(m_client, &PlainRpcClient::onDial, &PlainRpcClient::onHello,
                                      m_session.get()) == LP_OK;
}

char* PlainRpcClient::onDial(const char*, void* userData)
{
    return lp_string_copy(static_cast<Session*>(userData)->dial.c_str());
}

char* PlainRpcClient::onHello(const char*, void* userData)
{
    return lp_string_copy(static_cast<Session*>(userData)->hello.c_str());
}

PlainRpcClient::~PlainRpcClient()
{
    // Subscriptions must be gone before their owner. lp_unsubscribe guarantees
    // their callback storage is no longer reachable when it returns.
    m_subscriptions.clear();
    lp_client_destroy(m_client);
}

nlohmann::json PlainRpcClient::invoke(const std::string& method,
                                      const nlohmann::json& args,
                                      int timeoutMs,
                                      PlainRpcError* error)
{
    if (error) *error = {};
    if (!m_client) {
        if (error) *error = {"invalid_client", "RPC client is unavailable", {}};
        return nullptr;
    }

    char* result = nullptr;
    char* failure = nullptr;
    const std::string encoded = args.dump();
    const int status = lp_invoke(m_client, method.c_str(), encoded.c_str(),
                                 timeoutMs, &result, &failure);
    nlohmann::json value = status == LP_OK ? parseJson(result) : nlohmann::json(nullptr);
    if (status != LP_OK && error) *error = parseError(failure);
    lp_string_free(result);
    lp_string_free(failure);
    return value;
}

nlohmann::json PlainRpcClient::methods()
{
    if (!m_client) return nlohmann::json::array();
    char* encoded = lp_get_methods(m_client);
    nlohmann::json value = parseJson(encoded);
    lp_string_free(encoded);
    return value.is_array() ? value : nlohmann::json::array();
}

void PlainRpcClient::onEvent(const char* name, const char* dataJson, void* userData)
{
    auto* subscription = static_cast<Subscription*>(userData);
    nlohmann::json data = parseJson(dataJson);
    if (!data.is_array()) data = nlohmann::json::array();
    subscription->callback(name ? name : "", data);
}

bool PlainRpcClient::subscribe(
    const std::string& eventName,
    std::function<void(const std::string&, const nlohmann::json&)> callback)
{
    if (!m_client || !callback) return false;
    auto subscription = std::make_unique<Subscription>();
    subscription->callback = std::move(callback);
    subscription->handle = lp_subscribe(m_client, eventName.c_str(),
                                         &PlainRpcClient::onEvent,
                                         subscription.get());
    if (!subscription->handle) return false;
    std::lock_guard<std::mutex> lock(m_subscriptionsMutex);
    m_subscriptions.push_back(std::move(subscription));
    return true;
}

std::size_t PlainRpcClient::subscriptionCount() const
{
    std::lock_guard<std::mutex> lock(m_subscriptionsMutex);
    return m_subscriptions.size();
}

PlainRpcContext::PlainRpcContext(std::string origin,
                                 std::string capabilityTransport)
    : m_origin(std::move(origin))
    , m_capabilityTransport(std::move(capabilityTransport))
{
}

PlainRpcClient* PlainRpcContext::client(const std::string& target,
                                        const std::string& targetTransport)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto key = std::make_pair(target, targetTransport);
    auto found = m_clients.find(key);
    if (found != m_clients.end()) return found->second.get();
    auto created = std::make_unique<PlainRpcClient>(
        target, m_origin, targetTransport, m_capabilityTransport);
    if (!created->valid()) return nullptr;
    PlainRpcClient* result = created.get();
    m_clients.emplace(key, std::move(created));
    return result;
}

} // namespace logosctl
