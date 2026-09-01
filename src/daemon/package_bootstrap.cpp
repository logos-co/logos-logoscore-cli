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
// failing first entry cannot suppress a second one that would have loaded
// fine — and neither failure may skip the configuration that follows.
constexpr BundledModule kBundled[] = {
    {kPackageManager,
     "`package install`, `package ls`, `package rm` and `package info` will be "
     "unavailable in this session; catalog commands are unaffected"},
    {kPackageDownloader,
     "catalog commands (`package search`, `package download`) will be "
     "unavailable in this session; locally installed packages are unaffected"},
};

} // namespace

Outcome run(const Hooks& hooks, const Dirs& dirs,
            const std::string& signaturePolicy)
{
    Outcome out;

    for (const auto& m : kBundled) {
        if (!hooks.loadModule(m.name)) {
            hooks.warn(std::string("Warning: failed to load bundled module '")
                       + m.name + "'. " + m.lostCapability + ".");
            continue;
        }
        if (std::string_view(m.name) == kPackageManager) out.managerLoaded    = true;
        else                                             out.downloaderLoaded = true;
        if (hooks.note)
            hooks.note(std::string("Loaded bundled module: ") + m.name);
    }

    // Nothing loaded to configure. Not a half-configured state: an absent
    // package_manager enforces nothing and answers nothing.
    if (!out.managerLoaded)
        return out;

    bool allDelivered = true;
    auto set = [&](const char* method, std::vector<std::string> args) {
        const bool ok = hooks.configure(method, std::move(args));
        if (!ok) allDelivered = false;
        return ok;
    };

    // Embedded (read-only, ships with the binary) vs user (writable, this
    // session). The manager scans both and lets the user copy win on a name
    // collision, which is how a session can override a bundled module.
    if (!dirs.embeddedModules.empty()) {
        set("setEmbeddedModulesDirectory",   {dirs.embeddedModules});
        set("setEmbeddedUiPluginsDirectory", {dirs.embeddedUiPlugins});
    }
    set("setUserModulesDirectory",   {dirs.userModules});
    set("setUserUiPluginsDirectory", {dirs.userUiPlugins});

    // Trust is per-session: the keyring lives inside the config dir so that
    // copying a session carries its trust assumptions with it, and two
    // sessions can disagree about which signers they accept.
    set("setKeyringDirectory", {dirs.keyring});

    // ...and so is the policy applied to what those keys say about a package.
    // The module defaults to `warn`, so an unset `signature_policy:` is left
    // alone rather than restated; anything else is the operator asking for a
    // different answer and has to reach the module, or `require` would be a
    // setting that reads back correctly and enforces nothing.
    // The value is allowlisted by the config reader, so by here it is one of
    // none | warn | require.
    bool policyDelivered = true;
    if (!signaturePolicy.empty())
        policyDelivered = set("setSignaturePolicy", {signaturePolicy});

    out.directoriesSet = allDelivered;
    out.policyArmed    = !signaturePolicy.empty() && policyDelivered;

    // Fail closed when an explicitly configured policy did not land.
    //
    // The module's own default is `warn`: it prints a line for an unsigned
    // package and installs it anyway, and it accepts one signed by a key the
    // keyring does not trust. An operator who wrote `signature_policy:
    // require` gets neither rejection — while `logosctl config get` and
    // state.json keep reporting `require`, because that is what the file says.
    // A manager that enforces less than the session advertises is worse than
    // no manager, so take it out of the session rather than leave that gap
    // open behind a truthful-looking config.
    if (!signaturePolicy.empty() && !policyDelivered) {
        hooks.warn("Warning: could not deliver signature_policy='"
                   + signaturePolicy + "' to package_manager. Unloading it: "
                   "leaving it up would enforce the module default ('warn') "
                   "while this session's config advertises '" + signaturePolicy
                   + "'. Package commands are unavailable until the next "
                     "daemon start.");
        if (hooks.unloadModule) hooks.unloadModule(kPackageManager);
        out.managerLoaded   = false;
        out.managerDisabled = true;
        return out;
    }

    // A directory call that went missing is not a silent downgrade: every
    // directory in the module fails closed when unset — installs refuse with
    // "User modules directory is not set" and a scan of an unset tree returns
    // nothing. Loud, but not dangerous, so it warns instead of unloading.
    if (!allDelivered)
        hooks.warn("Warning: one or more package_manager directory settings did "
                   "not reach the module. `package ls` may report an empty "
                   "session and installs will refuse until the daemon is "
                   "restarted.");

    // A crash mid-dialog in a previous run can leave the module's single
    // gated-operation slot occupied, which would reject every subsequent
    // install. Basecamp clears it at startup for the same reason.
    hooks.configure("resetPendingAction", {});

    if (hooks.note)
        hooks.note("Configured package_manager: user=" + dirs.userModules
                   + " embedded=" + dirs.embeddedModules
                   + " keyring=" + dirs.keyring
                   + " signature_policy="
                   + (signaturePolicy.empty() ? "(module default)"
                                              : signaturePolicy));

    return out;
}

} // namespace package_bootstrap
