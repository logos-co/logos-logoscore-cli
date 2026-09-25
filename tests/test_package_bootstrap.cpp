#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "daemon/package_bootstrap.h"

namespace {

// Records what the bootstrap did, and lets a test choose which module fails to
// load and which call fails to reach the module.
struct Harness {
    std::set<std::string>    loadFailures;  // modules whose load returns false
    std::set<std::string>    callFailures;  // methods whose delivery fails

    std::vector<std::string> loadAttempts;
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
        h.call = [this](const std::string& method,
                        const std::vector<std::string>& args) {
            configured.emplace_back(method, args);
            return callFailures.count(method) == 0;
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

// -- the runtime's package config --------------------------------------------
//
// package_manager's settings answer the runtime only, so they travel with the
// runtime's configuration and land as the module loads: a downloader that
// fails to load can no longer skip them, and a policy that does not land fails
// package_manager's load in the runtime.

TEST(PackageBootstrap, PackageConfigCarriesTheSessionAndItsPolicy)
{
    EXPECT_EQ(package_bootstrap::packageConfig(sessionDirs(), "require"), (nlohmann::json{
        {"embedded_modules_dirs", {"/opt/logos/modules"}},
        {"embedded_ui_plugins_dirs", {"/opt/logos/plugins"}},
        {"user_modules_dir", "/home/u/.logosctl/modules"},
        {"user_ui_plugins_dir", "/home/u/.logosctl/plugins"},
        {"keyring_dir", "/home/u/.logosctl/keyring"},
        {"signature_policy", "require"},
    }));
}

TEST(PackageBootstrap, UnsetPolicyIsLeftToTheModuleDefault)
{
    EXPECT_FALSE(package_bootstrap::packageConfig(sessionDirs(), "").contains("signature_policy"));
}

TEST(PackageBootstrap, EmptyBundledDirLeavesOnlyTheEmbeddedEntriesOut)
{
    package_bootstrap::Dirs dirs = sessionDirs();
    dirs.embeddedModules.clear();
    dirs.embeddedUiPlugins.clear();

    const nlohmann::json config = package_bootstrap::packageConfig(dirs, "require");
    EXPECT_FALSE(config.contains("embedded_modules_dirs"));
    EXPECT_FALSE(config.contains("embedded_ui_plugins_dirs"));
    EXPECT_EQ(config.value("user_modules_dir", ""), "/home/u/.logosctl/modules");
    EXPECT_EQ(config.value("signature_policy", ""), "require");
}

// -- what runs as the shell ---------------------------------------------------

TEST(PackageBootstrap, LoadsBothAndClearsThePendingAction)
{
    Harness h;
    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_TRUE(out.managerLoaded);
    EXPECT_TRUE(out.downloaderLoaded);
    EXPECT_TRUE(out.pendingCleared);
    EXPECT_EQ(h.configured, (std::vector<std::pair<std::string, std::vector<std::string>>>{
        {"resetPendingAction", {}}}))
        << "nothing else: the runtime configures package_manager";
    EXPECT_TRUE(h.warnings.empty());
}

TEST(PackageBootstrap, DownloaderFailureStillClearsTheManager)
{
    Harness h;
    h.loadFailures.insert(kPd);

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_TRUE(out.managerLoaded);
    EXPECT_FALSE(out.downloaderLoaded);
    EXPECT_TRUE(h.called("resetPendingAction"));
}

// -- regression: the first entry must not suppress the second ---------------

TEST(PackageBootstrap, ManagerFailureStillAttemptsTheDownloader)
{
    Harness h;
    h.loadFailures.insert(kPm);

    const auto out = package_bootstrap::run(h.hooks(), sessionDirs(), "");

    EXPECT_EQ(h.loadAttempts, (std::vector<std::string>{kPm, kPd}));
    EXPECT_FALSE(out.managerLoaded);
    EXPECT_TRUE(out.downloaderLoaded);
    // An absent manager enforces nothing and answers nothing.
    EXPECT_TRUE(h.configured.empty());
}

TEST(PackageBootstrap, BothFailingWarnsAboutEachAndCallsNothing)
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

// The runtime refuses a package_manager that did not take the configured
// policy; the operator is told that is a possible reason.
TEST(PackageBootstrap, AManagerMissingUnderAPolicySaysWhyItMayBe)
{
    Harness h;
    h.loadFailures.insert(kPm);

    package_bootstrap::run(h.hooks(), sessionDirs(), "require");

    EXPECT_TRUE(h.warnedAbout("signature_policy='require'"));
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
