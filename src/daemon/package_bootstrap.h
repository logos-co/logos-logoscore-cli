#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

// Bringing up the two bundled package modules for this session.
//
// package_manager's settings (its directories, keyring and signature policy)
// answer the runtime only, so the daemon hands them over as the runtime's
// package config: liblogos applies it as package_manager loads, and refuses the
// load when a configured signature policy does not land. What stays here runs
// as the shell: loading both modules, and clearing an operation a crash left
// pending.
//
// The sequencing lives here, behind injected hooks, rather than inline in
// daemon.cpp, so what a load failure skips is testable without a runtime.
namespace package_bootstrap {

inline constexpr const char* kPackageManager    = "package_manager";
inline constexpr const char* kPackageDownloader = "package_downloader";

// Everything run() needs from the outside world.
struct Hooks {
    // core_service.loadModule(name, "required_and_optional"), as the shell.
    std::function<bool(const std::string& module)> loadModule;

    // One call into package_manager, as the shell. False when it did not reach
    // the module.
    std::function<bool(const std::string& method,
                       const std::vector<std::string>& args)> call;

    // Operator-visible warning, one line, no trailing newline.
    std::function<void(const std::string& line)> warn;

    // Same, but only wired up under --verbose. May be null.
    std::function<void(const std::string& line)> note;
};

// The session directories handed to package_manager. embedded* are the
// read-only tree beside the binary and may be empty (no bundled tree);
// user*/keyring are always set by the caller.
struct Dirs {
    std::string embeddedModules;
    std::string embeddedUiPlugins;
    std::string userModules;
    std::string userUiPlugins;
    std::string keyring;
};

// The runtime's package config for this session (logos_runtime_spawn's
// "package_config"). `signaturePolicy` is the operator's `signature_policy:`,
// empty when unset, which leaves the module's own default (warn).
nlohmann::json packageConfig(const Dirs& dirs, const std::string& signaturePolicy);

// What actually happened. Returned for tests and for the caller's log; the
// daemon does not branch on it, since nothing here may abort startup.
struct Outcome {
    bool managerLoaded    = false;  // package_manager is up, configured by the runtime
    bool downloaderLoaded = false;  // package_downloader is up
    bool pendingCleared   = false;  // resetPendingAction reached the manager
};

Outcome run(const Hooks& hooks, const Dirs& dirs, const std::string& signaturePolicy);

} // namespace package_bootstrap
