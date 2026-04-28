#include "client_connection.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace ClientConnection {

namespace {

// "core_service" → "LOGOSCORE_CLIENT_TRANSPORT_CORE_SERVICE". Hyphens
// fold to underscores so future module names with dashes (e.g. some
// future "logos-net") still resolve to a valid env-var name.
std::string moduleEnvVar(const std::string& moduleName)
{
    std::string upper;
    upper.reserve(moduleName.size());
    for (char c : moduleName) {
        if (c == '-') upper.push_back('_');
        else          upper.push_back(static_cast<char>(std::toupper(
                          static_cast<unsigned char>(c))));
    }
    return "LOGOSCORE_CLIENT_TRANSPORT_" + upper;
}

} // namespace

TransportInfo effectiveTransport(const TransportInfo& advertised)
{
    TransportInfo t = advertised;
    if (t.protocol != "tcp" && t.protocol != "tcp_ssl")
        return t;

    if (const char* h = std::getenv("LOGOSCORE_CLIENT_TCP_HOST"))
        if (*h) t.host = h;
    if (const char* p = std::getenv("LOGOSCORE_CLIENT_TCP_PORT")) {
        if (*p) {
            // Clamp silently to avoid UB on a nonsense value; the
            // subsequent connect will surface a clearer error than a
            // cryptic range exception here.
            try {
                int v = std::stoi(p);
                if (v > 0 && v <= 65535) t.port = static_cast<uint16_t>(v);
            } catch (...) {}
        }
    }
    if (const char* np = std::getenv("LOGOSCORE_CLIENT_NO_VERIFY_PEER"))
        if (*np) t.verifyPeer = false;
    return t;
}

std::optional<std::string> pickTransportForModule(
    const std::string& moduleName,
    const std::vector<TransportInfo>& advertised)
{
    if (advertised.empty()) return std::nullopt;

    // 1. Per-module env var wins. Empty value means "no override" so
    //    setting and unsetting work symmetrically — useful in test
    //    fixtures that want to clear an override.
    std::string preferred;
    if (const char* perModule = std::getenv(moduleEnvVar(moduleName).c_str())) {
        if (*perModule) preferred = perModule;
    }

    // 2. Process-wide default.
    if (preferred.empty()) {
        if (const char* global = std::getenv("LOGOSCORE_CLIENT_TRANSPORT")) {
            if (*global) preferred = global;
        }
    }

    // 3. Fall back to "local".
    if (preferred.empty()) preferred = "local";

    auto it = std::find_if(advertised.begin(), advertised.end(),
        [&](const TransportInfo& t) { return t.protocol == preferred; });
    if (it == advertised.end()) return std::nullopt;
    return it->protocol;
}

} // namespace ClientConnection
