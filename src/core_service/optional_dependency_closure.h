#pragma once

#include <nlohmann/json.hpp>
#include <functional>
#include <set>
#include <string>

namespace package_ops {

// Shared by install and upgrade planning. The supplied resolver also receives
// the installed set, so satisfied packages and unavailable offers stay out of
// the executable plan at every depth.
inline nlohmann::json resolveOptionalClosure(
    nlohmann::json inputs, bool withOptional,
    const std::function<nlohmann::json(const nlohmann::json&)>& resolve)
{
    using nlohmann::json;
    std::set<std::string> requested, selected, mandatory;
    for (const auto& input : inputs)
        requested.insert(input.is_string() ? input.get<std::string>() : input.value("name", std::string{}));
    selected = requested;
    bool firstPass = true;
    for (;;) {
        auto resolved = resolve(inputs);
        if (!resolved.is_array() || !withOptional) return resolved;
        for (const auto& entry : resolved) {
            if (entry.contains("error")) return resolved;
            if (firstPass) mandatory.insert(entry.value("name", std::string{}));
        }
        firstPass = false;
        bool added = false;
        for (const auto& entry : resolved)
            for (const auto& offer : entry.value("optionalDependencies", json::array())) {
                if (offer.contains("error")) continue;
                const std::string name = offer.value("name", std::string{});
                if (name.empty() || !selected.insert(name).second) continue;
                inputs.push_back(offer.at("request"));
                added = true;
            }
        if (added) continue;
        // Required dependencies of selected optionals are mandatory rows in
        // Basecamp too. Their branch is omitted by --no-optional, but while
        // selected its required children must be shown as required packages.
        for (const auto& entry : resolved) {
            const auto graph = entry.value("dependencyGraph", json::object());
            for (const auto& deps : graph)
                for (const auto& dep : deps)
                    mandatory.insert(dep.is_string() ? dep.get<std::string>() : dep.value("name", std::string{}));
        }
        for (auto& entry : resolved)
            entry["optional"] = !mandatory.count(entry.value("name", std::string{}));
        return resolved;
    }
}
} // namespace package_ops
