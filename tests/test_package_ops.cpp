#include <gtest/gtest.h>
#include "core_service/package_ops.h"

#include <map>
#include <set>

using nlohmann::json;
using package_ops::Op;

namespace {

// package_manager, package_downloader and the module runtime, in memory.
struct FakeModules {
    json installed = json::array();                    // getInstalledPackages
    std::function<json(const json& inputs)> resolve;   // resolveDependencies
    std::set<std::string> failDownload, failInstall;
    json extraDownloads = json::array();               // rows the resolver adds on download
    std::vector<std::string> loaded;

    std::vector<std::string> methods;                  // every call, in order
    std::vector<std::string> confirmedUpgrades, installedPaths, unloaded, reloaded;
    json downloadInputs;
    int refreshes = 0;

    package_ops::Backend backend()
    {
        package_ops::Backend b;
        b.call = [this](const char*, const std::string& method, const LogosList& args,
                        std::string*) -> json {
            methods.push_back(method);
            if (method == "getInstalledPackages") return installed;
            if (method == "resolveDependencies") return resolve(json::parse(args[0].get<std::string>()));
            if (method == "confirmUpgrade") confirmedUpgrades.push_back(args[0].get<std::string>());
            if (method == "downloadResolvedDependencies") {
                downloadInputs = json::parse(args[0].get<std::string>());
                json rows = extraDownloads;
                for (const auto& in : downloadInputs) {
                    const std::string name = in["name"].get<std::string>();
                    rows.push_back(failDownload.count(name)
                        ? json{{"name", name}, {"error", "unreachable"}}
                        : json{{"name", name}, {"path", "/dl/" + name + ".lgx"}});
                }
                return rows;
            }
            if (method == "installPlugin") {
                const std::string path = args[0].get<std::string>();
                for (const auto& f : failInstall)
                    if (path == "/dl/" + f + ".lgx") return json{{"error", "untrusted signer"}};
                installedPaths.push_back(path);
                return json{{"success", true}};
            }
            return json{{"success", true}};   // request*/ack/confirm*/cancel*
        };
        b.loadedModules = [this] { return loaded; };
        b.unloadModule = [this](const std::string& m) { unloaded.push_back(m); };
        b.loadModule = [this](const std::string& m) { reloaded.push_back(m); return true; };
        b.refreshModules = [this] { ++refreshes; };
        return b;
    }
};

json row(const std::string& name, const std::string& version, bool topLevel = false)
{
    return {{"name", name}, {"version", version}, {"rootHash", name + "@" + version},
            {"repositoryUrl", "https://repo/"}, {"topLevel", topLevel}};
}

json installedRow(const std::string& name, const std::string& version)
{
    return {{"name", name}, {"version", version}, {"hashes", {{"root", name + "@" + version}}}};
}

// S offers optional O; selecting O adds it as a top-level row.
std::function<json(const json&)> subjectWithOptional(const std::string& version = "2.0.0")
{
    return [version](const json& inputs) {
        auto s = row("S", version, true);
        s["optionalDependencies"] = json::array({json{{"name", "O"}, {"requiredBy", "S"},
            {"request", {{"name", "O"}, {"version", "1.0.0"}, {"optional", true}}}}});
        json out = json::array({s});
        if (inputs.size() > 1) out.push_back(row("O", "1.0.0", true));
        return out;
    };
}

std::vector<std::string> names(const json& arr)
{
    std::vector<std::string> out;
    for (const auto& e : arr) out.push_back(e.is_string() ? e.get<std::string>() : e.value("name", ""));
    return out;
}

} // namespace

TEST(PackageOpsApply, UpgradeKeepsANamedPackageThatIsAlreadyCurrent)
{
    FakeModules pm;
    pm.installed = json::array({installedRow("S", "2.0.0")});
    pm.resolve = subjectWithOptional("2.0.0");
    auto backend = pm.backend();
    const auto result = package_ops::apply(backend, Op::Upgrade, {"S"}, {});
    ASSERT_EQ(result.value("status", ""), "ok") << result.dump();
    // confirmUpgrade uninstalls the user copy; nothing would reinstall a current S.
    EXPECT_TRUE(pm.confirmedUpgrades.empty());
    EXPECT_EQ(pm.installedPaths, std::vector<std::string>{"/dl/O.lgx"});
}

TEST(PackageOpsApply, UpgradeStillReplacesAChangedNamedPackage)
{
    FakeModules pm;
    pm.installed = json::array({installedRow("S", "1.0.0")});
    pm.resolve = subjectWithOptional("2.0.0");
    auto backend = pm.backend();
    const auto result = package_ops::apply(backend, Op::Upgrade, {"S"}, {});
    ASSERT_EQ(result.value("status", ""), "ok") << result.dump();
    EXPECT_EQ(pm.confirmedUpgrades, std::vector<std::string>{"S"});
    EXPECT_EQ(pm.installedPaths, (std::vector<std::string>{"/dl/S.lgx", "/dl/O.lgx"}));
}

TEST(PackageOpsApply, DownloadRequestsMarkOnlyOptionalRows)
{
    FakeModules pm;
    pm.resolve = subjectWithOptional();
    auto backend = pm.backend();
    package_ops::apply(backend, Op::Install, {"S"}, {});
    ASSERT_EQ(pm.downloadInputs.size(), 2u);
    for (const auto& in : pm.downloadInputs)
        EXPECT_EQ(in.value("optional", false), in["name"] == "O") << in.dump();
}

TEST(PackageOpsApply, FailedOptionalDownloadIsSkippedAndModulesAreRestored)
{
    FakeModules pm;
    pm.installed = json::array({installedRow("S", "1.0.0")});
    pm.loaded = {"S"};
    pm.resolve = subjectWithOptional("2.0.0");
    pm.failDownload = {"O"};
    auto backend = pm.backend();
    const auto result = package_ops::apply(backend, Op::Upgrade, {"S"}, {});
    ASSERT_EQ(result.value("status", ""), "ok") << result.dump();
    EXPECT_EQ(names(result["installed"]), std::vector<std::string>{"S"});
    EXPECT_EQ(names(result["skipped_optional"]), std::vector<std::string>{"O"});
    EXPECT_EQ(pm.refreshes, 1);
    EXPECT_EQ(pm.reloaded, std::vector<std::string>{"S"});
}

TEST(PackageOpsApply, FailedOptionalInstallIsSkipped)
{
    FakeModules pm;
    pm.resolve = subjectWithOptional();
    pm.failInstall = {"O"};
    auto backend = pm.backend();
    const auto result = package_ops::apply(backend, Op::Install, {"S"}, {});
    ASSERT_EQ(result.value("status", ""), "ok") << result.dump();
    EXPECT_EQ(names(result["installed"]), std::vector<std::string>{"S"});
    ASSERT_EQ(result["skipped_optional"].size(), 1u);
    EXPECT_EQ(result["skipped_optional"][0]["error"], "untrusted signer");
}

TEST(PackageOpsApply, FailedRequiredDownloadStillFails)
{
    FakeModules pm;
    pm.resolve = subjectWithOptional();
    pm.failDownload = {"S"};
    auto backend = pm.backend();
    const auto result = package_ops::apply(backend, Op::Install, {"S"}, {});
    EXPECT_EQ(result.value("status", ""), "error");
    EXPECT_EQ(result.value("failed_step", ""), "download");
}

TEST(PackageOpsApply, NoDepsInstallsOnlyThePlannedRows)
{
    FakeModules pm;
    pm.resolve = [](const json&) { return json::array({row("D", "1.0.0"), row("S", "1.0.0", true)}); };
    pm.extraDownloads = json::array({json{{"name", "D"}, {"path", "/dl/D.lgx"}}});
    auto backend = pm.backend();
    package_ops::Options opts;
    opts.withDeps = false;
    const auto result = package_ops::apply(backend, Op::Install, {"S"}, opts);
    ASSERT_EQ(result.value("status", ""), "ok") << result.dump();
    EXPECT_EQ(pm.installedPaths, std::vector<std::string>{"/dl/S.lgx"});
}
