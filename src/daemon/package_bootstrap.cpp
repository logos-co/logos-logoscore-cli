#include "package_bootstrap.h"

#include <string_view>

namespace package_bootstrap {
namespace {

struct BundledModule {
    const char* name;
    // What specifically stops working without this one. The warning used to
    // say "package commands will be unavailable" for either module, which is
    // wrong in both directions: package_manager failing still leaves the
    // catalog reachable, and package_downloader failing leaves every local
    // package command working.
    const char* lostCapability;
};

// Order here is presentation only. Each entry is loaded independently, so a
// failing first entry cannot suppress a second one that would have loaded fine.
constexpr BundledModule kBundled[] = {
    {kPackageManager,
     "`package install`, `package ls`, `package rm` and `package info` will be "
     "unavailable in this session; catalog commands are unaffected"},
    {kPackageDownloader,
     "catalog commands (`package search`, `package download`) will be "
     "unavailable in this session; locally installed packages are unaffected"},
};

} // namespace

nlohmann::json packageConfig(const Dirs& dirs, const std::string& signaturePolicy)
{
    // Trust is per-session: the keyring lives inside the config dir, so copying
    // a session carries its trust assumptions with it.
    nlohmann::json config = {
        {"user_modules_dir", dirs.userModules},
        {"user_ui_plugins_dir", dirs.userUiPlugins},
        {"keyring_dir", dirs.keyring},
    };
    // Embedded (read-only, ships with the binary) vs user (writable, this
    // session): the manager scans both, and the user copy wins a name clash.
    if (!dirs.embeddedModules.empty()) {
        config["embedded_modules_dirs"] = nlohmann::json::array({dirs.embeddedModules});
        config["embedded_ui_plugins_dirs"] = nlohmann::json::array({dirs.embeddedUiPlugins});
    }
    // Unset is left to the module; anything else must land, or the runtime
    // does not load package_manager at all.
    if (!signaturePolicy.empty()) config["signature_policy"] = signaturePolicy;
    return config;
}

Outcome run(const Hooks& hooks, const Dirs& dirs, const std::string& signaturePolicy)
{
    Outcome out;

    for (const auto& m : kBundled) {
        if (!hooks.loadModule(m.name)) {
            std::string line = std::string("Warning: failed to load bundled module '") + m.name
                + "'. " + m.lostCapability + ".";
            if (std::string_view(m.name) == kPackageManager && !signaturePolicy.empty())
                line += " The runtime does not load a package_manager that refuses signature_policy='"
                    + signaturePolicy + "'; its log says why.";
            hooks.warn(line);
            continue;
        }
        if (std::string_view(m.name) == kPackageManager) out.managerLoaded    = true;
        else                                             out.downloaderLoaded = true;
        if (hooks.note)
            hooks.note(std::string("Loaded bundled module: ") + m.name);
    }

    // Nothing loaded to clear. An absent package_manager enforces nothing and
    // answers nothing.
    if (!out.managerLoaded)
        return out;

    // A crash mid-dialog in a previous run can leave the module's single
    // gated-operation slot occupied, which would reject every subsequent
    // install. Basecamp clears it at startup for the same reason.
    out.pendingCleared = hooks.call("resetPendingAction", {});

    if (hooks.note)
        hooks.note("package_manager takes this session's settings from the runtime: user="
                   + dirs.userModules + " embedded=" + dirs.embeddedModules
                   + " keyring=" + dirs.keyring + " signature_policy="
                   + (signaturePolicy.empty() ? "(module default)" : signaturePolicy));

    return out;
}

} // namespace package_bootstrap
