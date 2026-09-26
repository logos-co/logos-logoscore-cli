#include "remote.h"

#include "../config.h"

#ifdef LOGOSCTL_HAS_REMOTE
#include <logos/peering/local_identity.h>
#include <logos/peering/service.h>

#include <filesystem>
#include <memory>
#include <thread>
#include <unistd.h>
#endif

namespace logosctl::remote {

std::string inviteText(const std::string& text)
{
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_object() && doc.contains("invite") && doc["invite"].is_string())
        return doc["invite"].get<std::string>();
    return text;
}

#ifdef LOGOSCTL_HAS_REMOTE

namespace {

using json = nlohmann::json;
using logos::peering::CallerRef;
using logos::peering::LocalIdentity;
using logos::peering::PeeringService;

// The client manages its own peering state, as the shell a runtime names.
const CallerRef kSelf = CallerRef::module("logosctl");

std::string hostName()
{
    char name[256] = {};
    if (gethostname(name, sizeof name - 1) != 0) return {};
    std::string host(name);
    if (const auto dot = host.find('.'); dot != std::string::npos) host.resize(dot);
    return host;
}

// libpeering in this process: no endpoint of its own, only outbound links.
std::unique_ptr<PeeringService> open(std::string* error)
{
    const std::filesystem::path dir = std::filesystem::u8path(Config::configDir()) / "remote";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    PeeringService::Options options;
    options.stateDir = dir;
    options.identity = std::make_shared<LocalIdentity>(dir / "identity");
    options.tick = std::chrono::milliseconds(200);
    auto service = std::make_unique<PeeringService>(std::move(options));
    const std::string host = hostName();
    std::string name = host.empty() ? "logosctl" : "logosctl on " + host;
    if (name.size() > 64) name.resize(64);
    const json configured = service->invoke(CallerRef::host(), "configure",
                                            json::array({{{"name", name}, {"shell", "logosctl"}}}));
    if (configured.contains("error")) {
        if (error) *error = configured.value("error", std::string("peering refused its configuration"));
        return nullptr;
    }
    return service;
}

bool failed(const json& reply, std::string* error)
{
    if (!reply.is_object() || !reply.contains("error")) return false;
    if (error) *error = reply["error"].is_string() ? reply["error"].get<std::string>() : reply["error"].dump();
    return true;
}

json peerDoc(PeeringService& service, const std::string& runtimeId)
{
    for (const auto& p : service.invoke(kSelf, "peers", json::array()).value("peers", json::array()))
        if (p.value("runtime_id", "") == runtimeId) return p;
    return json::object();
}

} // namespace

bool available() { return true; }

std::optional<json> self(std::string* error)
{
    auto service = open(error);
    if (!service) return std::nullopt;
    const json status = service->invoke(kSelf, "status", json::array());
    if (failed(status, error)) return std::nullopt;
    return json{{"runtime_id", status.value("runtime_id", "")},
                {"display_id", status.value("display_id", "")},
                {"name", status.value("name", "")}};
}

std::optional<json> peers(std::string* error)
{
    auto service = open(error);
    if (!service) return std::nullopt;
    const json reply = service->invoke(kSelf, "peers", json::array());
    if (failed(reply, error)) return std::nullopt;
    return reply.value("peers", json::array());
}

std::optional<json> pair(const std::string& invite, std::chrono::seconds limit,
                         const std::function<void(const json&)>& waiting, std::string* error)
{
    auto service = open(error);
    if (!service) return std::nullopt;
    const json started = service->invoke(kSelf, "redeemInvite", json::array({invite}));
    if (failed(started, error)) return std::nullopt;
    const std::string id = started.value("id", "");
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::string shown;
    while (std::chrono::steady_clock::now() < deadline) {
        json mine = json::object();
        for (const auto& p : service->invoke(kSelf, "pending", json::array()).value("pending", json::array()))
            if (p.value("id", "") == id) mine = p;
        const std::string state = mine.value("state", "");
        if (state != shown && waiting) waiting(mine);
        shown = state;
        if (state == "paired") return peerDoc(*service, mine.value("peer_runtime_id", ""));
        if (state == "failed" || mine.empty()) {
            if (error) *error = mine.value("error", std::string("the daemon refused the pairing"));
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (error) *error = "the daemon did not accept this client in time";
    return std::nullopt;
}

bool remove(const std::string& peer, std::string* error)
{
    auto service = open(error);
    return service && !failed(service->invoke(kSelf, "removePeer", json::array({peer})), error);
}

std::optional<Route> route(const std::string& peer, std::string* error)
{
    auto service = open(error);
    if (!service) return std::nullopt;
    auto granted = service->operatorRoute(peer, error);
    if (!granted) return std::nullopt;
    return Route{std::move(granted->dial), std::move(granted->hello), std::move(granted->chainPem),
                 std::move(granted->keyPem)};
}

#else

namespace {
constexpr const char* kUnavailable =
    "this logosctl was built without Remote Runtime Control (libpeering)";

template <typename T>
std::optional<T> unavailable(std::string* error)
{
    if (error) *error = kUnavailable;
    return std::nullopt;
}
} // namespace

bool available() { return false; }
std::optional<nlohmann::json> self(std::string* error) { return unavailable<nlohmann::json>(error); }
std::optional<nlohmann::json> peers(std::string* error) { return unavailable<nlohmann::json>(error); }
std::optional<nlohmann::json> pair(const std::string&, std::chrono::seconds,
                                   const std::function<void(const nlohmann::json&)>&, std::string* error)
{
    return unavailable<nlohmann::json>(error);
}
bool remove(const std::string&, std::string* error)
{
    if (error) *error = kUnavailable;
    return false;
}
std::optional<Route> route(const std::string&, std::string* error) { return unavailable<Route>(error); }

#endif

} // namespace logosctl::remote
