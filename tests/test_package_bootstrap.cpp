#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "daemon/package_bootstrap.h"

namespace {

// Records what the bootstrap did, and lets a test choose which module fails to
// load and which configuration call fails to reach the module.
struct Harness {
    std::set<std::string>    loadFailures;      // modules whose load returns false
    std::set<std::string>    configureFailures; // methods whose delivery fails

    std::vector<std::string> loadAttempts;
    std::vector<std::string> unloaded;
    std::vector<std::pair<std::string, std::vector<std::string>>> configured;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;

    package_bootstrap::Hooks hooks()
    {
        package_bootstrap::Hooks h;
        h.loadModule = [this](const std::string& m) {
            loadAttempts.push_back(m);
            return loadFailures.count(m) == 0;
        };
        h.unloadModule = [this](const std::string& m) { unloaded.push_back(m); };
        h.configure = [this](const std::string& method,
                             const std::vector<std::string>& args) {
            configured.emplace_back(method, args);
            return configureFailures.count(method) == 0;
        };
        h.warn = [this](const std::string& line) { warnings.push_back(line); };
        h.note = [this](const std::string& line) { notes.push_back(line); };
        return h;
    }

    bool called(const std::string& method) const
    {
        return std::any_of(configured.begin(), configured.end(),
                           [&](const auto& c) { return c.first == method; });
    }

    std::vector<std::string> argsOf(const std::string& method) const
    {
        for (const auto& c : configured)
            if (c.first == method) return c.second;
        return {};
    }

    bool warnedAbout(const std::string& needle) const
    {
        return std::any_of(warnings.begin(), warnings.end(),
                           [&](const std::string& w) {
                               return w.find(needle) != std::string::npos;
                           });
    }
};

package_bootstrap::Dirs sessionDirs()
{
    package_bootstrap::Dirs d;
    d.embeddedModules   = "/opt/logos/modules";
    d.embeddedUiPlugins = "/opt/logos/plugins";
    d.userModules       = "/home/u/.logosctl/modules";
    d.userUiPlugins     = "/home/u/.logosctl/plugins";
    d.keyring           = "/home/u/.logosctl/keyring";
    return d;
}

const char* kPm = package_bootstrap::kPackageManager;
const char* kPd = package_bootstrap::kPackageDownloader;

} // namespace

// -- happy path -------------------------------------------------------------

TEST(PackageBootstrap, LoadsBothAndConfiguresEverything)
{
    Harness h;
    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_TRUE(out.managerLoaded);
    EXPECT_TRUE(out.downloaderLoaded);
    EXPECT_TRUE(out.directoriesSet);
    EXPECT_TRUE(out.policyArmed);
    EXPECT_FALSE(out.managerDisabled);

    EXPECT_TRUE(h.called("setEmbeddedModulesDirectory"));
    EXPECT_TRUE(h.called("setEmbeddedUiPluginsDirectory"));
    EXPECT_TRUE(h.called("setUserModulesDirectory"));
    EXPECT_TRUE(h.called("setUserUiPluginsDirectory"));
    EXPECT_TRUE(h.called("setKeyringDirectory"));
    EXPECT_TRUE(h.called("setSignaturePolicy"));
    EXPECT_TRUE(h.called("resetPendingAction"));
    EXPECT_TRUE(h.warnings.empty());

    EXPECT_EQ(h.argsOf("setUserModulesDirectory"),
              (std::vector<std::string>{"/home/u/.logosctl/modules"}));
    EXPECT_EQ(h.argsOf("setKeyringDirectory"),
              (std::vector<std::string>{"/home/u/.logosctl/keyring"}));
    EXPECT_EQ(h.argsOf("setSignaturePolicy"),
              (std::vector<std::string>{"require"}));
}

TEST(PackageBootstrap, UnsetPolicyIsLeftToTheModuleDefault)
{
    Harness h;
    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "");

    EXPECT_FALSE(h.called("setSignaturePolicy"));
    EXPECT_FALSE(out.policyArmed);
    EXPECT_FALSE(out.managerDisabled);
    EXPECT_TRUE(out.managerLoaded);
}

TEST(PackageBootstrap, EmptyBundledDirSkipsOnlyTheEmbeddedCalls)
{
    Harness h;
    package_bootstrap::Dirs dirs = sessionDirs();
    dirs.embeddedModules.clear();
    dirs.embeddedUiPlugins.clear();

    const auto out = package_bootstrap::run(h.hooks(), dirs, "require");

    EXPECT_FALSE(h.called("setEmbeddedModulesDirectory"));
    EXPECT_FALSE(h.called("setEmbeddedUiPluginsDirectory"));
    EXPECT_TRUE(h.called("setUserModulesDirectory"));
    EXPECT_TRUE(out.policyArmed);
}

// -- regression: a downloader failure must not skip configuration -----------
//
// The loop used to `return` on the first load failure, which happened BEFORE
// every set*Directory / setSignaturePolicy call below it. package_manager came
// up loaded and completely unconfigured on any platform where the second
// module failed to load, not just Windows.

TEST(PackageBootstrap, DownloaderFailureStillConfiguresTheManager)
{
    Harness h;
    h.loadFailures.insert(kPd);

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_TRUE(out.managerLoaded);
    EXPECT_FALSE(out.downloaderLoaded);

    EXPECT_TRUE(h.called("setEmbeddedModulesDirectory"));
    EXPECT_TRUE(h.called("setUserModulesDirectory"));
    EXPECT_TRUE(h.called("setUserUiPluginsDirectory"));
    EXPECT_TRUE(h.called("setKeyringDirectory"));
    EXPECT_TRUE(h.called("resetPendingAction"));
}

// The specific security consequence of the old early return: the operator's
// `signature_policy: require` never reached the module, which then ran at its
// own default (`warn`) — unsigned packages install with a printed warning, and
// a package signed by an untrusted key installs too — while state.json and
// `logosctl config get` kept reporting `require`.
TEST(PackageBootstrap, DownloaderFailureStillArmsTheSignaturePolicy)
{
    Harness h;
    h.loadFailures.insert(kPd);

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    ASSERT_TRUE(h.called("setSignaturePolicy"));
    EXPECT_EQ(h.argsOf("setSignaturePolicy"),
              (std::vector<std::string>{"require"}));
    EXPECT_TRUE(out.policyArmed);
    EXPECT_TRUE(out.managerLoaded);
    EXPECT_FALSE(out.managerDisabled);
}

// -- regression: the first entry must not suppress the second ---------------

TEST(PackageBootstrap, ManagerFailureStillAttemptsTheDownloader)
{
    Harness h;
    h.loadFailures.insert(kPm);

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_EQ(h.loadAttempts, (std::vector<std::string>{kPm, kPd}));
    EXPECT_FALSE(out.managerLoaded);
    EXPECT_TRUE(out.downloaderLoaded);

    // Nothing to configure, and nothing to fail closed on: an absent manager
    // enforces nothing and answers nothing.
    EXPECT_TRUE(h.configured.empty());
    EXPECT_TRUE(h.unloaded.empty());
    EXPECT_FALSE(out.managerDisabled);
}

TEST(PackageBootstrap, BothFailingWarnsAboutEachAndConfiguresNothing)
{
    Harness h;
    h.loadFailures.insert(kPm);
    h.loadFailures.insert(kPd);

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_EQ(h.warnings.size(), 2u);
    EXPECT_TRUE(h.configured.empty());
    EXPECT_FALSE(out.managerLoaded);
    EXPECT_FALSE(out.downloaderLoaded);
}

// -- regression: the warning must name what actually went away --------------
//
// Both failures used to emit the same line: "Package commands will be
// unavailable in this session." That is wrong for either module on its own.

TEST(PackageBootstrap, WarningNamesOnlyTheLostCapability)
{
    {
        Harness h;
        h.loadFailures.insert(kPd);
        package_bootstrap::run(h.hooks(), sessionDirs(), "");

        ASSERT_EQ(h.warnings.size(), 1u);
        const std::string& w = h.warnings.front();
        EXPECT_NE(w.find(kPd), std::string::npos);
        EXPECT_NE(w.find("package search"), std::string::npos);
        // The manager DID load, so the session's local package commands work.
        EXPECT_NE(w.find("locally installed packages are unaffected"),
                  std::string::npos);
        EXPECT_EQ(w.find("`package install`"), std::string::npos);
    }
    {
        Harness h;
        h.loadFailures.insert(kPm);
        package_bootstrap::run(h.hooks(), sessionDirs(), "");

        ASSERT_EQ(h.warnings.size(), 1u);
        const std::string& w = h.warnings.front();
        EXPECT_NE(w.find(kPm), std::string::npos);
        EXPECT_NE(w.find("`package install`"), std::string::npos);
        EXPECT_NE(w.find("catalog commands are unaffected"), std::string::npos);
    }
}

// -- fail closed on an undelivered policy -----------------------------------

TEST(PackageBootstrap, UndeliveredPolicyUnloadsTheManager)
{
    Harness h;
    h.configureFailures.insert("setSignaturePolicy");

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_EQ(h.unloaded, (std::vector<std::string>{kPm}));
    EXPECT_TRUE(out.managerDisabled);
    EXPECT_FALSE(out.managerLoaded);
    EXPECT_FALSE(out.policyArmed);
    EXPECT_TRUE(h.warnedAbout("signature_policy='require'"));

    // Do not leave the gate open and then clear the pending-action slot as if
    // the manager were in service.
    EXPECT_FALSE(h.called("resetPendingAction"));
}

TEST(PackageBootstrap, UndeliveredPolicyWithNoPolicyConfiguredIsNotFatal)
{
    Harness h;
    // No `signature_policy:` in the config, so the call is never made and
    // there is nothing to fail closed on — the module's own default applies,
    // which is what the session advertises.
    h.configureFailures.insert("setSignaturePolicy");

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "");

    EXPECT_TRUE(h.unloaded.empty());
    EXPECT_TRUE(out.managerLoaded);
    EXPECT_FALSE(out.managerDisabled);
}

// A missing directory is loud but safe: the module fails closed on every unset
// directory (install refuses with "User modules directory is not set"), so it
// warns rather than taking the manager out of the session.
TEST(PackageBootstrap, UndeliveredDirectoryWarnsButKeepsTheManager)
{
    Harness h;
    h.configureFailures.insert("setUserModulesDirectory");

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_TRUE(h.unloaded.empty());
    EXPECT_TRUE(out.managerLoaded);
    EXPECT_FALSE(out.directoriesSet);
    EXPECT_TRUE(out.policyArmed);
    EXPECT_TRUE(h.warnedAbout("directory settings did not reach the module"));
}

// A null note hook (the non-verbose daemon) must not be called.
TEST(PackageBootstrap, VerboseNotesAreOptional)
{
    Harness h;
    package_bootstrap::Hooks hooks = h.hooks();
    hooks.note = nullptr;

    const auto out = package_bootstrap::run(hooks, sessionDirs(), "require");

    EXPECT_TRUE(out.managerLoaded);
    EXPECT_TRUE(h.notes.empty());
}
