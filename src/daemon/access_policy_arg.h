#ifndef LOGOSCORE_DAEMON_ACCESS_POLICY_ARG_H
#define LOGOSCORE_DAEMON_ACCESS_POLICY_ARG_H

#include <optional>
#include <string>

namespace logoscore {

// The bare deny-by-default document. `mode` is the runtime's own switch (see
// liblogos access_policy.h): "enforce" turns restrictions into denials, and
// with no explicit `restrictions` the runtime derives them from the declared
// dependency graph — a module may only call the modules it declared. This
// spelling exists so an operator can arm that without hand-writing JSON; it is
// NOT a second switch, it expands to exactly this document.
inline constexpr const char* kEnforceAlias   = "enforce";
inline constexpr const char* kEnforceEnvelope =
    R"({"version":1,"mode":"enforce","restrictions":{}})";

// Resolve the operator's --access-policy argument into the JSON document
// handed to logos_core_set_access_policy():
//
//   "enforce"              -> kEnforceEnvelope (deny-by-default)
//   text starting with '{' -> inline JSON, used as-is
//   anything else          -> a path to a JSON file, read from disk
//
// The result is parse-checked (schema enforcement is the runtime's job).
// Returns nullopt on a path that cannot be opened or content that is not valid
// JSON, with a human-readable reason in `error` when non-null.
//
// Absent the flag entirely, the daemon installs no policy at all — enforcement
// off, which is the pre-existing behaviour. This function is only reached when
// the operator asked for something.
std::optional<std::string> resolveAccessPolicyArg(const std::string& arg,
                                                  std::string* error = nullptr);

} // namespace logoscore

#endif // LOGOSCORE_DAEMON_ACCESS_POLICY_ARG_H
