#pragma once

#include <functional>
#include <string>
#include <vector>

// Bringing up the two bundled package modules and pointing package_manager at
// this session's directories.
//
// The decision logic lives here, behind injected hooks, rather than inline in
// daemon.cpp: what a load failure skips — and what it must NOT skip — is the
// part that has been wrong, and it is not reachable from a test through
// logos_core_load_module and a live socket.
namespace package_bootstrap {

inline constexpr const char* kPackageManager    = "package_manager";
inline constexpr const char* kPackageDownloader = "package_downloader";

// Everything run() needs from the outside world.
struct Hooks {
    // logos_core_load_module(name, with_dependencies=true).
    std::function<bool(const std::string& module)> loadModule;

    // logos_core_unload_module(name, with_dependents=true). Called only to
    // fail closed — see the signature-policy handling in run().
    std::function<void(const std::string& module)> unloadModule;

    // One configuration call into package_manager. Returns false when the
    // call demonstrably did not reach the module (no client for it, or the
    // remote object could not be acquired); true when it dispatched. Every
    // method used here takes zero or one string argument.
    std::function<bool(const std::string& method,
                       const std::vector<std::string>& args)> configure;

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

// What actually happened. Returned for tests and for the caller's log; the
// daemon does not branch on it, since nothing here is allowed to abort
// startup.
struct Outcome {
    bool managerLoaded     = false;  // package_manager is up and usable
    bool downloaderLoaded  = false;  // package_downloader is up
    bool directoriesSet    = false;  // every directory call was delivered
    bool policyArmed       = false;  // an explicit policy was configured AND delivered
    bool managerDisabled   = false;  // loaded, then unloaded to fail closed
};

// `signaturePolicy` is the operator's `signature_policy:` — empty when unset,
// otherwise one of none | warn | require (the config reader allowlists it).
Outcome run(const Hooks& hooks, const Dirs& dirs,
            const std::string& signaturePolicy);

} // namespace package_bootstrap
