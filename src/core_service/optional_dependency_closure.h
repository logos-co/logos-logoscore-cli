#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace package_ops {

// Shared by install and upgrade planning. The supplied resolver also receives
// the installed set, so satisfied packages and unavailable offers stay out of
// the executable plan at every depth.
//
// Same selection rule as Basecamp's OptionalDependencyPreview: every available
// offer is selected unless the package is already a required dependency of
// something in the plan, in which case it installs as that dependency instead
// of as a pinned root.
//
// `selectNew` false leaves optionals that are not installed unselected, as
// Basecamp does for a package that is already installed.
inline nlohmann::json resolveOptionalClosure(
    nlohmann::json inputs, bool withOptional,
    const std::function<nlohmann::json(const nlohmann::json&)>& resolve,
    bool selectNew = true)
{
    using nlohmann::json;
    auto nameOf = [](const json& entry) {
        return entry.is_string() ? entry.get<std::string>()
             : entry.is_object() ? entry.value("name", std::string{}) : std::string{};
    };
    std::set<std::string> requested;
    for (const auto& input : inputs) requested.insert(nameOf(input));
    std::vector<std::pair<std::string, json>> offered;  // first available request per name, in discovery order
    std::set<std::string> offeredNames;
    std::set<std::string> seenRequests;
    json current = inputs;
    for (;;) {
        auto resolved = resolve(current);
        if (!resolved.is_array() || !withOptional) return resolved;
        for (const auto& entry : resolved)
            if (entry.contains("error")) return resolved;

        std::set<std::string> children;
        std::map<std::string, std::set<std::string>> graph;
        for (const auto& entry : resolved) {
            for (const auto& offer : entry.value("optionalDependencies", json::array())) {
                const std::string name = offer.value("name", std::string{});
                if (offer.contains("error") || name.empty() || requested.count(name)) continue;
                if (!selectNew && !offer.contains("installedVersion")) continue;
                // An installed optional at its installed release is kept as it is.
                if (offer.contains("installedVersion")
                    && offer.value("version", std::string{}) == offer.value("installedVersion", std::string{}))
                    continue;
                if (offeredNames.insert(name).second) offered.emplace_back(name, offer.at("request"));
            }
            const auto edges = entry.value("dependencyGraph", json::object());
            for (auto it = edges.begin(); it != edges.end(); ++it)
                for (const auto& dep : it.value()) {
                    children.insert(nameOf(dep));
                    graph[it.key()].insert(nameOf(dep));
                }
        }

        json next = inputs;
        std::set<std::string> selected;
        for (const auto& [name, request] : offered)
            if (!children.count(name)) { next.push_back(request); selected.insert(name); }
        if (next == current) {
            auto reach = [&](const std::set<std::string>& roots) {
                std::set<std::string> seen;
                std::vector<std::string> stack(roots.begin(), roots.end());
                while (!stack.empty()) {
                    const std::string n = stack.back(); stack.pop_back();
                    if (!seen.insert(n).second) continue;
                    for (const auto& d : graph[n]) stack.push_back(d);
                }
                return seen;
            };
            const auto mandatory = reach(requested);
            for (auto& entry : resolved) {
                const std::string name = entry.value("name", std::string{});
                entry["optional"] = selected.count(name) > 0;
                // Required only by selected optionals: "only mandatory" drops it too.
                if (selected.count(name) || mandatory.count(name)) continue;
                json requiredFor = json::array();
                for (const auto& o : selected)
                    if (reach({o}).count(name)) requiredFor.push_back(o);
                if (!requiredFor.empty()) entry["requiredFor"] = requiredFor;
            }
            return resolved;
        }
        // A selection that keeps flipping falls back to the required plan.
        if (!seenRequests.insert(next.dump()).second) return resolve(inputs);
        current = std::move(next);
    }
}
} // namespace package_ops
