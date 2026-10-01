#pragma once

#include <nlohmann/json.hpp>
#include <functional>
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
            for (const auto& deps : entry.value("dependencyGraph", json::object()))
                for (const auto& dep : deps) children.insert(nameOf(dep));
        }

        json next = inputs;
        std::set<std::string> selected;
        for (const auto& [name, request] : offered)
            if (!children.count(name)) { next.push_back(request); selected.insert(name); }
        if (next == current) {
            for (auto& entry : resolved)
                entry["optional"] = selected.count(entry.value("name", std::string{})) > 0;
            return resolved;
        }
        // A selection that keeps flipping falls back to the required plan.
        if (!seenRequests.insert(next.dump()).second) return resolve(inputs);
        current = std::move(next);
    }
}
} // namespace package_ops
