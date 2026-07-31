#include "package_ops.h"
#include "config.h"
#include "logos_core.h"

#include <logos_api.h>
#include <logos_api_client.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

namespace package_ops {
namespace {

constexpr const char* kPm = "package_manager";
constexpr const char* kPd = "package_downloader";

LogosMap err(const std::string& code, const std::string& message)
{
    return LogosMap{{"status", "error"}, {"code", code}, {"message", message}};
}

// Thin call helper. Returns a null json when the module isn't reachable, which
// every caller treats as a hard failure — there is no partial-success path
// through a module we cannot talk to.
nlohmann::json call(LogosAPI* api, const char* module, const std::string& method,
                    const LogosList& args = LogosList::array())
{
    if (!api) return nullptr;
    LogosAPIClient* client = api->getClient(module);
    if (!client) return nullptr;
    return client->invokeRemoteMethod(module, method, args);
}

// The package modules answer mutating calls with {success, error?}. Anything
// else — a null from an unreachable module, or a dispatch_failed envelope —
// is a failure too.
bool ok(const nlohmann::json& r, std::string* errorOut = nullptr)
{
    if (!r.is_object()) {
        if (errorOut) *errorOut = "module did not respond";
        return false;
    }
    if (r.value("success", false)) return true;
    if (errorOut) {
        *errorOut = r.value("error", r.value("message", std::string("unknown error")));
    }
    return false;
}

std::vector<std::string> loadedModules()
{
    std::vector<std::string> out;
    char** mods = logos_core_get_loaded_modules();
    if (!mods) return out;
    for (int i = 0; mods[i]; ++i) {
        out.emplace_back(mods[i]);
        free(mods[i]);
    }
    free(mods);
    return out;
}

LogosList installedPackages(LogosAPI* api)
{
    nlohmann::json r = call(api, kPm, "getInstalledPackages");
    return r.is_array() ? r : LogosList::array();
}

// [{name, version, rootHash}] — what the resolver needs to short-circuit
// dependencies that are already satisfied on disk, so it doesn't propose
// upgrading something the user never asked about.
std::string installedPackagesJson(const LogosList& installed)
{
    LogosList out = LogosList::array();
    for (const auto& p : installed) {
        out.push_back(LogosMap{
            {"name",     p.value("name", std::string{})},
            {"version",  p.value("version", std::string{})},
            {"rootHash", p.contains("hashes") && p["hashes"].is_object()
                             ? p["hashes"].value("root", std::string{})
                             : std::string{}},
        });
    }
    return out.dump();
}

// The manifest dependency form the resolver expects: a bare name, or
// {name, version} when the caller pinned one.
std::string dependenciesJson(const std::vector<std::string>& names,
                             const Options& opts)
{
    LogosList deps = LogosList::array();
    for (const auto& n : names) {
        if (opts.version.empty()) {
            deps.push_back(n);
        } else {
            deps.push_back(LogosMap{{"name", n}, {"version", opts.version}});
        }
    }
    return deps.dump();
}

// Same classification basecamp's confirmation dialog uses
// (PackageCoordinator::depAction). "reinstall" is the same-version,
// different-root-hash case — a rebuild of the identical version, which is a
// real change even though the version string doesn't move.
std::string depAction(const std::string& installedVersion,
                      const std::string& resolvedVersion,
                      const std::string& installedHash,
                      const std::string& resolvedHash)
{
    if (installedVersion.empty())            return "install";
    if (installedVersion != resolvedVersion) return "upgrade";
    if (!resolvedHash.empty() && !installedHash.empty()
        && installedHash != resolvedHash)    return "reinstall";
    return "installed";
}

struct InstalledInfo {
    std::string version;
    std::string rootHash;
};

std::map<std::string, InstalledInfo> byName(const LogosList& installed)
{
    std::map<std::string, InstalledInfo> m;
    for (const auto& p : installed) {
        InstalledInfo i;
        i.version = p.value("version", std::string{});
        if (p.contains("hashes") && p["hashes"].is_object())
            i.rootHash = p["hashes"].value("root", std::string{});
        m[p.value("name", std::string{})] = std::move(i);
    }
    return m;
}

// Which of `affected` are running right now. These get stopped before the
// files move and restarted afterwards; everything else is untouched.
std::vector<std::string> affectedLoaded(const std::vector<std::string>& affected)
{
    const auto loaded = loadedModules();
    const std::unordered_set<std::string> loadedSet(loaded.begin(), loaded.end());
    std::vector<std::string> out;
    for (const auto& n : affected)
        if (loadedSet.count(n)) out.push_back(n);
    return out;
}

// Resolve the install/upgrade closure. Returns the resolver's array, or a
// null json on failure.
nlohmann::json resolveClosure(LogosAPI* api,
                              const std::vector<std::string>& names,
                              const Options& opts,
                              const LogosList& installed)
{
    if (!opts.withDeps) {
        // --no-deps: act only on what was named. Still shaped like resolver
        // output so the rest of the pipeline is identical.
        nlohmann::json r = call(api, kPd, "resolveDependencies",
                                LogosList{dependenciesJson(names, opts), std::string{}});
        if (!r.is_array()) return nullptr;
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& e : r)
            if (e.value("topLevel", false)) filtered.push_back(e);
        return filtered;
    }
    return call(api, kPd, "resolveDependencies",
                LogosList{dependenciesJson(names, opts),
                          installedPackagesJson(installed)});
}

// The cascade set for a removal: the package plus everything that depends on
// it, dependents first so nothing is removed while something still needs it.
std::vector<std::string> removalSet(LogosAPI* api, const std::string& name,
                                    bool withDependents, std::string* errorOut)
{
    if (!withDependents) return {name};

    nlohmann::json r = call(api, kPm, "resolveFlatDependents",
                            LogosList{name, true});
    if (!r.is_array()) {
        if (errorOut) *errorOut = "could not resolve dependents of '" + name + "'";
        return {};
    }
    std::vector<std::string> out;
    for (const auto& d : r) {
        const std::string dn = d.value("name", std::string{});
        if (!dn.empty() && dn != name) out.push_back(dn);
    }
    // Dependents first, target last.
    out.push_back(name);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// plan
// ---------------------------------------------------------------------------

LogosMap plan(LogosAPI* api, Op op,
              const std::vector<std::string>& names,
              const Options& opts)
{
    LogosMap out;
    out["status"] = "ok";
    out["op"] = (op == Op::Install ? "install"
                 : op == Op::Upgrade ? "upgrade" : "remove");

    const LogosList installed = installedPackages(api);
    const auto installedByName = byName(installed);

    LogosList changes = LogosList::array();
    std::vector<std::string> affected;

    if (op == Op::Remove) {
        for (const auto& n : names) {
            if (!installedByName.count(n))
                return err("PACKAGE_NOT_INSTALLED",
                           "Package '" + n + "' is not installed.");
            std::string e;
            auto set = removalSet(api, n, opts.withDependents, &e);
            if (set.empty()) return err("RESOLVE_FAILED", e);
            for (const auto& r : set) {
                if (std::find(affected.begin(), affected.end(), r) != affected.end())
                    continue;
                affected.push_back(r);
                auto it = installedByName.find(r);
                changes.push_back(LogosMap{
                    {"name", r},
                    {"action", "remove"},
                    {"fromVersion", it != installedByName.end() ? it->second.version : ""},
                    {"toVersion", ""},
                });
            }
        }
    } else {
        // Local .lgx files bypass the catalog entirely — inspect each one so
        // the plan still shows what it would do.
        if (!opts.localFiles.empty()) {
            for (const auto& f : opts.localFiles) {
                nlohmann::json info = call(api, kPm, "inspectPackage", LogosList{f});
                if (!info.is_object())
                    return err("INSPECT_FAILED", "Could not inspect '" + f + "'.");
                if (info.contains("error") && !info.value("error", std::string{}).empty())
                    return err("INSPECT_FAILED", info.value("error", std::string{}));
                const std::string n = info.value("name", std::string{});
                const std::string v = info.value("version", std::string{});
                const std::string iv = info.value("installedVersion", std::string{});
                affected.push_back(n);
                changes.push_back(LogosMap{
                    {"name", n},
                    {"action", info.value("isAlreadyInstalled", false)
                                   ? (iv == v ? "reinstall" : "upgrade") : "install"},
                    {"fromVersion", iv},
                    {"toVersion", v},
                    {"file", f},
                    {"signature", info.value("signatureStatus", std::string{})},
                });
            }
        } else {
            nlohmann::json resolved = resolveClosure(api, names, opts, installed);
            if (!resolved.is_array())
                return err("RESOLVE_FAILED",
                           "Could not resolve packages from the catalog. "
                           "Check that a catalog is configured and reachable "
                           "(`catalog ls`, `catalog refresh`).");

            for (const auto& e : resolved) {
                // The resolver reports unsatisfiable constraints inline.
                if (e.contains("error")) {
                    return err("RESOLVE_FAILED",
                               "Cannot resolve '" + e.value("name", std::string("?"))
                               + "': " + e.value("error", std::string{}));
                }
                const std::string n = e.value("name", std::string{});
                const std::string rv = e.value("version", std::string{});
                const std::string rh = e.value("rootHash", std::string{});
                auto it = installedByName.find(n);
                const std::string iv = it != installedByName.end() ? it->second.version : "";
                const std::string ih = it != installedByName.end() ? it->second.rootHash : "";
                const std::string action = depAction(iv, rv, ih, rh);

                changes.push_back(LogosMap{
                    {"name", n},
                    {"action", action},
                    {"fromVersion", iv},
                    {"toVersion", rv},
                    {"repository", e.value("repositoryUrl", std::string{})},
                    {"topLevel", e.value("topLevel", false)},
                });
                // A package already at the resolved version isn't touched, so
                // it doesn't need stopping and restarting.
                if (action != "installed") affected.push_back(n);
            }
        }
    }

    out["changes"] = changes;
    out["affected_loaded"] = affectedLoaded(affected);
    return out;
}

// ---------------------------------------------------------------------------
// apply
// ---------------------------------------------------------------------------

namespace {

// Open the gate and acknowledge it in one breath. The module starts a 3s
// ack timer inside requestX and cancels the whole operation if nothing
// acknowledges; in-process that window is never at risk, but the ack is still
// required — confirmX refuses an un-acked pending action.
bool openGate(LogosAPI* api, Op op, const std::string& name,
              const std::string& depChangesJson, std::string* errorOut)
{
    nlohmann::json r;
    switch (op) {
    case Op::Install:
        r = call(api, kPm, "requestInstall",
                 LogosList{name, std::string{}, std::string{}, depChangesJson});
        break;
    case Op::Upgrade:
        r = call(api, kPm, "requestUpgrade",
                 LogosList{name, std::string{}, 0, depChangesJson});
        break;
    case Op::Remove:
        r = call(api, kPm, "requestUninstall", LogosList{name});
        break;
    }
    if (!ok(r, errorOut)) return false;

    nlohmann::json ackR = call(api, kPm, "ackPendingAction", LogosList{name});
    if (!ok(ackR, errorOut)) return false;
    return true;
}

void closeGate(LogosAPI* api, Op op, const std::string& name)
{
    switch (op) {
    case Op::Install: call(api, kPm, "cancelInstall",   LogosList{name}); break;
    case Op::Upgrade: call(api, kPm, "cancelUpgrade",   LogosList{name, std::string{}}); break;
    case Op::Remove:  call(api, kPm, "cancelUninstall", LogosList{name}); break;
    }
}

} // namespace

LogosMap apply(LogosAPI* api, Op op,
               const std::vector<std::string>& names,
               const Options& opts)
{
    LogosMap result = plan(api, op, names, opts);
    if (result.value("status", std::string{}) == "error")
        return result;

    // Which running modules this will disturb. Captured before anything moves
    // so the restore set reflects the pre-operation world.
    std::vector<std::string> toRestore;
    for (const auto& n : result["affected_loaded"])
        toRestore.push_back(n.get<std::string>());

    auto fail = [&](const std::string& step, const std::string& message) {
        result["status"] = "error";
        result["failed_step"] = step;
        result["error"] = message;
        return result;
    };

    const std::string depChanges = result["changes"].dump();

    // ── Remove ──────────────────────────────────────────────────────────────
    if (op == Op::Remove) {
        std::vector<std::string> victims;
        for (const auto& c : result["changes"])
            victims.push_back(c.value("name", std::string{}));
        if (victims.empty())
            return fail("plan", "nothing to remove");

        // Build the single vector<string> argument explicitly. Brace-init
        // (LogosList{victims}) does NOT wrap a std::vector the way it wraps a
        // scalar — it yields an empty args array, and the module then sees a
        // zero-argument call it cannot dispatch.
        LogosList victimNames = LogosList::array();
        for (const auto& v : victims) victimNames.push_back(v);
        const LogosList victimArgs = LogosList::array({victimNames});

        std::string e;
        // The batch gate exists precisely so a cascade is one confirmation and
        // one destructive section rather than N independent ones.
        nlohmann::json r = call(api, kPm, "requestMultiUninstall", victimArgs);
        if (!ok(r, &e)) return fail("request", e);
        nlohmann::json ackR = call(api, kPm, "ackPendingAction",
                                   LogosList::array({victims.front()}));
        if (!ok(ackR, &e)) {
            call(api, kPm, "cancelMultiUninstall", victimArgs);
            return fail("ack", e);
        }

        // Stop them before their files disappear, dependents first — which is
        // the order `victims` is already in.
        for (const auto& v : victims)
            logos_core_unload_module(v.c_str(), /*with_dependents=*/true);

        nlohmann::json confirmR = call(api, kPm, "confirmMultiUninstall", victimArgs);
        if (!ok(confirmR, &e)) return fail("confirm", e);

        logos_core_refresh_modules();
        result["removed"] = victims;
        // Nothing to restore: every affected module was just removed.
        result["reloaded"] = LogosList::array();
        return result;
    }

    // ── Install / Upgrade ───────────────────────────────────────────────────
    LogosList installedNow = LogosList::array();

    // Local files: no catalog, no resolution — install exactly what was given.
    if (!opts.localFiles.empty()) {
        for (const auto& c : result["changes"]) {
            const std::string name = c.value("name", std::string{});
            const std::string file = c.value("file", std::string{});
            std::string e;
            if (!openGate(api, op, name, depChanges, &e))
                return fail("request", e);

            for (const auto& m : toRestore)
                logos_core_unload_module(m.c_str(), /*with_dependents=*/true);

            nlohmann::json confirmR = (op == Op::Upgrade)
                ? call(api, kPm, "confirmUpgrade", LogosList{name, std::string{}})
                : call(api, kPm, "confirmInstall", LogosList{name});
            if (!ok(confirmR, &e)) { closeGate(api, op, name); return fail("confirm", e); }

            nlohmann::json ins = call(api, kPm, "installPlugin", LogosList{file, false});
            if (!ins.is_object() || !ins.value("error", std::string{}).empty()) {
                return fail("install", ins.is_object()
                    ? ins.value("error", std::string("install failed"))
                    : "package_manager did not respond");
            }
            installedNow.push_back(name);
        }
    } else {
        // Gate per top-level package: the module allows exactly one pending
        // operation globally, so a multi-package request has to be sequenced.
        for (const auto& name : names) {
            std::string e;
            if (!openGate(api, op, name, depChanges, &e))
                return fail("request", e);

            for (const auto& m : toRestore)
                logos_core_unload_module(m.c_str(), /*with_dependents=*/true);

            // confirmUpgrade removes the old copy in-module; confirmInstall
            // has nothing to remove. Either way the download+install below is
            // the initiator's job — that is what the approval hands back.
            nlohmann::json confirmR = (op == Op::Upgrade)
                ? call(api, kPm, "confirmUpgrade", LogosList{name, std::string{}})
                : call(api, kPm, "confirmInstall", LogosList{name});
            if (!ok(confirmR, &e)) { closeGate(api, op, name); return fail("confirm", e); }
        }

        const LogosList installed = installedPackages(api);
        nlohmann::json downloaded = call(api, kPd, "downloadResolvedDependencies",
                                         LogosList{dependenciesJson(names, opts),
                                                   opts.withDeps
                                                       ? installedPackagesJson(installed)
                                                       : std::string{}});
        if (!downloaded.is_array())
            return fail("download", "package_downloader did not return a download set");

        for (const auto& d : downloaded) {
            if (d.contains("error") && !d.value("error", std::string{}).empty())
                return fail("download", d.value("name", std::string("?")) + ": "
                                        + d.value("error", std::string{}));
            const std::string path = d.value("path", std::string{});
            if (path.empty()) continue;   // already satisfied, nothing fetched

            nlohmann::json ins = call(api, kPm, "installPlugin", LogosList{path, false});
            if (!ins.is_object() || !ins.value("error", std::string{}).empty()) {
                return fail("install", d.value("name", std::string("?")) + ": "
                    + (ins.is_object() ? ins.value("error", std::string("install failed"))
                                       : "package_manager did not respond"));
            }
            installedNow.push_back(d.value("name", std::string{}));
        }
    }

    // Make the new files discoverable without a restart. Without this the
    // daemon's known-module set still reflects the pre-install scan, so a
    // freshly installed module could not be loaded at all.
    logos_core_refresh_modules();

    // Restart what was running before — and only that. A newly installed
    // package is left unloaded: installing puts files on disk, loading is a
    // separate explicit act.
    LogosList reloaded = LogosList::array();
    for (const auto& m : toRestore) {
        if (logos_core_load_module(m.c_str(), /*with_dependencies=*/true))
            reloaded.push_back(m);
    }

    result["installed"] = installedNow;
    result["reloaded"] = reloaded;
    return result;
}

LogosMap download(LogosAPI* api, const std::string& name,
                  const Options& opts, const std::string& destDir)
{
    nlohmann::json r = call(api, kPd, "downloadPinned",
                            LogosList{opts.catalog, name, opts.version, opts.rootHash});
    if (!r.is_object())
        return err("DOWNLOAD_FAILED", "package_downloader did not respond");

    const std::string error = r.value("error", std::string{});
    std::string path = r.value("path", std::string{});
    if (!error.empty() || path.empty()) {
        return err("DOWNLOAD_FAILED",
                   error.empty() ? ("could not download '" + name + "'") : error);
    }

    // package_downloader has no destination parameter, so the file is sitting
    // in $TMPDIR. Move it where it was actually asked to go.
    const std::string dir = destDir.empty() ? Config::downloadsDir() : destDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return err("DOWNLOAD_FAILED",
                   "downloaded '" + name + "' but could not create " + dir +
                   ": " + ec.message());
    }

    const fs::path from(path);
    const fs::path to = fs::path(dir) / from.filename();
    if (fs::equivalent(from.parent_path(), fs::path(dir), ec)) {
        // Already in place (a downloader that gained a destination, or a cache
        // dir that happens to be $TMPDIR). Nothing to move.
        ec.clear();
    } else {
        fs::rename(from, to, ec);
        if (ec) {
            // rename() fails across filesystems, which is the normal case when
            // the cache dir and $TMPDIR are on different mounts. Copy instead,
            // and only drop the original once the copy is safely there.
            ec.clear();
            fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                return err("DOWNLOAD_FAILED",
                           "downloaded '" + name + "' to " + path +
                           " but could not place it in " + dir + ": " + ec.message());
            }
            std::error_code rmec;
            fs::remove(from, rmec);   // best effort; the copy is what matters
        }
        path = to.string();
    }

    LogosMap res = LogosMap::object();
    for (auto it = r.begin(); it != r.end(); ++it) res[it.key()] = it.value();
    res["path"] = path;
    return LogosMap{{"status", "ok"}, {"result", res}};
}

} // namespace package_ops
