#ifndef LOGOSCTL_REMOTE_H
#define LOGOSCTL_REMOTE_H

// Remote Runtime Control (`logosctl --remote PEER`): this client's own peering
// identity and the daemons that enrolled it for runtime-control, kept in
// <config dir>/remote, and a session with a daemon's core_service for each run,
// as the consumer `logosctl`. Needs libpeering; without it every call says so.

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace logosctl::remote {

bool available();

// An invite as a file or stdin holds it: bare, or the JSON `peer invite`
// prints when piped.
std::string inviteText(const std::string& text);

// This client, as the daemons it pairs with see it: {runtime_id, display_id, name}.
std::optional<nlohmann::json> self(std::string* error);
// Its pairings: [{runtime_id, alias, display_name, display_id, granted_uses, status, …}].
std::optional<nlohmann::json> peers(std::string* error);

// Redeems a runtime-control invite; `waiting` hears each state until the daemon
// accepts (or refuses) it. The new peer, or nothing.
std::optional<nlohmann::json> pair(const std::string& invite, std::chrono::seconds limit,
                                   const std::function<void(const nlohmann::json&)>& waiting,
                                   std::string* error);
bool remove(const std::string& peer, std::string* error);

struct Route {
    nlohmann::json dial;
    nlohmann::json hello;
    std::string chainPem;
    std::string keyPem;
};
std::optional<Route> route(const std::string& peer, std::string* error);

} // namespace logosctl::remote

#endif
