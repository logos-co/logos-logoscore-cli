#include "package_command.h"

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace {

constexpr const char* kPm = "package_manager";
constexpr const char* kPd = "package_downloader";

// The package modules speak JSON-in-a-string for their list-shaped arguments,
// so a JSON array has to be dumped rather than passed as a real array.
std::string jsonArg(const LogosList& v) { return v.dump(); }

bool endsWithLgx(const std::string& s)
{
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".lgx") == 0;
}

// A destructive operation with no `-y` and no terminal to ask on is refused
// rather than assumed-yes: a script that forgot --yes should fail loudly, not
// silently uninstall things.
bool confirm(const std::string& prompt)
{
    if (!isatty(STDIN_FILENO)) return false;
    std::cout << prompt << " [y/N] " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    return line == "y" || line == "Y" || line == "yes" || line == "Yes";
}

const char* actionVerb(const std::string& action)
{
    if (action == "install")   return "install";
    if (action == "upgrade")   return "upgrade";
    if (action == "reinstall") return "reinstall";
    if (action == "remove")    return "remove";
    return "keep";
}

} // namespace

int PackageCommand::execute(const std::vector<std::string>& args)
{
    if (args.empty()) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl package <install|upgrade|remove|ls|show|deps|search|download> ...");
        return 1;
    }

    const std::string sub = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "install")   return mutate("install", rest);
    if (sub == "upgrade")   return mutate("upgrade", rest);
    if (sub == "remove" || sub == "uninstall") return mutate("remove", rest);
    if (sub == "ls" || sub == "list") return list(rest);
    if (sub == "show" || sub == "info") return show(rest);
    if (sub == "deps")      return deps(rest);
    if (sub == "search")    return search(rest);
    if (sub == "download")  return download(rest);

    output().printError("INVALID_ARGS", "Unknown package subcommand: " + sub);
    return 1;
}

// ── install / upgrade / remove ───────────────────────────────────────────────

int PackageCommand::mutate(const std::string& op, const std::vector<std::string>& args)
{
    CLI::App cli{"package " + op};
    cli.set_help_flag();
    std::vector<std::string> names;
    cli.add_option("names", names, "Package name(s), or path(s) to .lgx files");
    std::string file, dir, version, rootHash, catalog;
    cli.add_option("--file", file, "Install from a local .lgx file");
    cli.add_option("--dir", dir, "Install every .lgx in a directory");
    cli.add_option("--version", version, "Pin an exact version");
    cli.add_option("--root-hash", rootHash, "Disambiguate releases sharing a version");
    cli.add_option("--catalog", catalog, "Restrict to one catalog (url or name)");
    bool yes = false, dryRun = false, noDeps = false, noDependents = false;
    cli.add_flag("-y,--yes", yes, "Do not prompt for confirmation");
    cli.add_flag("--dry-run", dryRun, "Show what would change and stop");
    cli.add_flag("--no-deps", noDeps, "Do not pull in dependencies");
    cli.add_flag("--no-dependents", noDependents, "Do not remove dependents");

    try {
        parseArgs(cli, args);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl package " + op + " <name|file.lgx ...> "
            "[--version V] [-y] [--dry-run]");
        return 1;
    }

    // `remove` names an installed package; there is nothing on disk for it to
    // read. Both flags were accepted and then ignored by the daemon, so the
    // command reported "already up to date" and removed nothing.
    if (op == "remove" && (!file.empty() || !dir.empty())) {
        output().printError("INVALID_ARGS",
            "--file / --dir apply to install and upgrade. "
            "Give remove the package name.");
        return 1;
    }

    // Expand --file / --dir / positional paths into concrete paths up front so
    // the daemon is handed a settled list rather than re-deriving it.
    LogosList localFiles = LogosList::array();
    auto addFile = [&](const std::string& p) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(p, ec)) {
            output().printError("INVALID_ARGS", "No such .lgx file: " + p);
            return false;
        }
        localFiles.push_back(std::filesystem::absolute(p).string());
        return true;
    };

    // A positional argument ending in `.lgx` is a path, not a catalog name.
    // `package show` has read it that way all along, and the catalog cannot
    // hold a name with a `.lgx` suffix anyway, so the alternative reading was
    // never useful -- it just sent the path to the resolver, which failed with
    // "no candidate matches './foo.lgx'" and read like the file was rejected.
    if (op != "remove") {
        std::vector<std::string> catalogNames;
        for (const auto& n : names) {
            if (!endsWithLgx(n)) { catalogNames.push_back(n); continue; }
            if (!addFile(n)) return 1;
        }
        names = catalogNames;
    }

    if (!file.empty() && !addFile(file)) return 1;
    if (!dir.empty()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            output().printError("INVALID_ARGS", "Not a directory: " + dir);
            return 1;
        }
        // Count what the directory itself contributed: sharing the emptiness
        // test with --file let an empty --dir pass unreported.
        const size_t before = localFiles.size();
        for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            if (e.is_regular_file() && e.path().extension() == ".lgx")
                localFiles.push_back(std::filesystem::absolute(e.path()).string());
        if (localFiles.size() == before) {
            output().printError("INVALID_ARGS", "No .lgx files found in: " + dir);
            return 1;
        }
    }

    if (names.empty() && localFiles.empty()) {
        output().printError("INVALID_ARGS",
            op == "remove"
                ? std::string("Nothing to remove. Name an installed package.")
                : "Nothing to " + op + ". Name a package, or give a path to an "
                  ".lgx file (or --file / --dir).");
        return 1;
    }

    // The daemon plans one way or the other -- local files bypass the catalog
    // entirely -- so a request carrying both silently dropped the named
    // packages and installed only the files. Refuse it instead of doing half
    // of what was asked.
    if (!names.empty() && !localFiles.empty()) {
        output().printError("INVALID_ARGS",
            "Cannot mix catalog packages with local .lgx files in one "
            + op + ". Run them as separate commands.");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0) return err;

    LogosMap opts{
        {"withDeps", !noDeps},
        {"withDependents", !noDependents},
        {"version", version},
        {"rootHash", rootHash},
        {"catalog", catalog},
        {"localFiles", localFiles},
    };
    LogosList nameList = LogosList::array();
    for (const auto& n : names) nameList.push_back(n);

    LogosMap plan = client().planPackageOperation(op, nameList, opts);
    if (plan.value("status", std::string{}) == "error") {
        output().printError(plan.value("code", std::string("PLAN_FAILED")),
                            plan.value("message", std::string{}), plan);
        return 1;
    }

    // Nothing to do is a success, not an error — re-running an install that
    // already happened should be a no-op, the way apt treats it.
    const auto& changes = plan["changes"];
    bool anyChange = false;
    for (const auto& c : changes)
        if (c.value("action", std::string{}) != "installed") anyChange = true;

    if (!anyChange) {
        if (output().isJsonMode()) output().printSuccess(plan);
        else output().printRaw("Nothing to do — already up to date.");
        return 0;
    }

    if (output().isJsonMode() && dryRun) {
        output().printSuccess(plan);
        return 0;
    }

    if (!output().isJsonMode()) {
        output().printRaw(fmt::format("The following changes will be made ({}):", op));
        for (const auto& c : changes) {
            const std::string action = c.value("action", std::string{});
            if (action == "installed") continue;
            const std::string from = c.value("fromVersion", std::string{});
            const std::string to   = c.value("toVersion", std::string{});
            output().printRaw(fmt::format("  {:<10} {}{}",
                actionVerb(action),
                c.value("name", std::string{}),
                to.empty() ? (from.empty() ? "" : fmt::format(" ({})", from))
                           : (from.empty() ? fmt::format(" {}", to)
                                           : fmt::format(" {} -> {}", from, to))));
        }
        const auto& affected = plan["affected_loaded"];
        if (affected.is_array() && !affected.empty()) {
            std::vector<std::string> a;
            for (const auto& m : affected) a.push_back(m.get<std::string>());
            // A removal stops them for good; an install/upgrade puts them
            // back. Saying "restarted" in both cases would be a lie in one.
            output().printRaw(fmt::format(
                op == "remove"
                    ? "These running modules will be stopped: {}"
                    : "These running modules will be stopped and restarted: {}",
                fmt::join(a, ", ")));
        }
    }

    if (dryRun) {
        if (!output().isJsonMode()) output().printRaw("(dry run — nothing was changed)");
        return 0;
    }

    if (!yes && !confirm("Proceed?")) {
        output().printError("CANCELLED",
            isatty(STDIN_FILENO) ? "Cancelled."
                                 : "Refusing to proceed without confirmation. Pass -y to continue.");
        return 1;
    }

    LogosMap result = client().applyPackageOperation(op, nameList, opts);
    if (result.value("status", std::string{}) == "error") {
        // A failure arrives in one of two shapes, and reading only the first
        // threw away the reason. package_ops' own step failures carry
        // `failed_step` + `error`; anything that went wrong before the chain
        // started -- an unreachable module, a dead daemon, a transport error --
        // carries `code` + `message` instead. Reporting that second shape with
        // the first shape's keys printed "install failed at step '?': " with
        // nothing after the colon, which is how a real diagnosis was lost.
        const std::string step   = result.value("failed_step", std::string{});
        std::string       reason = result.value("error", std::string{});
        if (reason.empty()) reason = result.value("message", std::string{});
        if (reason.empty()) reason = "no reason reported";

        output().printError(result.value("code", std::string("PACKAGE_OP_FAILED")),
                            step.empty()
                                ? fmt::format("{} failed: {}", op, reason)
                                : fmt::format("{} failed at step '{}': {}", op, step, reason),
                            result);
        return 1;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
        return 0;
    }

    auto names_of = [](const nlohmann::json& arr) {
        std::vector<std::string> v;
        if (arr.is_array()) for (const auto& e : arr) v.push_back(e.get<std::string>());
        return v;
    };
    const auto installed = names_of(result.value("installed", LogosList::array()));
    const auto removed   = names_of(result.value("removed",   LogosList::array()));
    const auto reloaded  = names_of(result.value("reloaded",  LogosList::array()));

    if (!installed.empty())
        output().printRaw(fmt::format("Installed: {}", fmt::join(installed, ", ")));
    if (!removed.empty())
        output().printRaw(fmt::format("Removed: {}", fmt::join(removed, ", ")));
    if (!reloaded.empty())
        output().printRaw(fmt::format("Restarted: {}", fmt::join(reloaded, ", ")));
    // Installing does not load — say so, or the next `call` failing is a
    // mystery. Name what the user asked for, not the first dependency that
    // happened to install ahead of it.
    if (!installed.empty()) {
        const std::string hint = names.empty() ? installed.front() : names.front();
        output().printRaw(fmt::format("Load with: logosctl module load {}", hint));
    }
    return 0;
}

// ── ls ───────────────────────────────────────────────────────────────────────

int PackageCommand::list(const std::vector<std::string>& args)
{
    CLI::App cli{"package ls"};
    cli.set_help_flag();
    std::string type;
    cli.add_option("--type", type, "Filter by type: core | ui");
    try { parseArgs(cli, args); }
    catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logosctl package ls [--type core|ui]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0) return err;

    const char* method = type == "ui"   ? "getInstalledUiPlugins"
                       : type == "core" ? "getInstalledModules"
                                        : "getInstalledPackages";
    LogosMap r = client().callModuleMethod(kPm, method, LogosList::array());
    if (r.value("status", std::string{}) == "error") {
        output().printError(r.value("code", std::string("RPC_FAILED")),
                            r.value("message", std::string{}), r);
        return 1;
    }

    const auto& pkgs = r["result"];
    if (output().isJsonMode()) { output().printRaw(pkgs.dump()); return 0; }

    if (!pkgs.is_array() || pkgs.empty()) {
        output().printRaw("No packages installed.");
        return 0;
    }
    output().printRaw(fmt::format("{:<28} {:<12} {:<10} {}", "NAME", "VERSION", "TYPE", "SOURCE"));
    for (const auto& p : pkgs) {
        output().printRaw(fmt::format("{:<28} {:<12} {:<10} {}",
            p.value("name", std::string{}),
            p.value("version", std::string("-")),
            p.value("type", std::string("-")),
            p.value("installType", std::string("-"))));
    }
    return 0;
}

// ── show ─────────────────────────────────────────────────────────────────────

int PackageCommand::show(const std::vector<std::string>& args)
{
    if (args.empty()) {
        output().printError("INVALID_ARGS", "Usage: logosctl package show <name|file.lgx>");
        return 1;
    }
    const std::string target = args[0];

    int err = ensureConnected();
    if (err != 0) return err;

    // A path to an .lgx is inspected on disk; anything else is looked up
    // among the installed packages. One verb, because "tell me about this
    // package" is one question whether or not it is installed yet.
    if (endsWithLgx(target)) {
        LogosMap r = client().callModuleMethod(kPm, "inspectPackage",
                        LogosList{std::filesystem::absolute(target).string()});
        if (r.value("status", std::string{}) == "error") {
            output().printError(r.value("code", std::string("RPC_FAILED")),
                                r.value("message", std::string{}), r);
            return 1;
        }
        if (output().isJsonMode()) { output().printRaw(r["result"].dump()); return 0; }
        const auto& i = r["result"];
        for (const char* k : {"name", "version", "type", "category", "description",
                              "signatureStatus", "signerDid", "signerName",
                              "isAlreadyInstalled", "installedVersion"}) {
            if (i.contains(k) && !i[k].is_null())
                output().printRaw(fmt::format("{:<20} {}", std::string(k) + ":",
                                              i[k].is_string() ? i[k].get<std::string>()
                                                               : i[k].dump()));
        }
        return 0;
    }

    LogosMap r = client().callModuleMethod(kPm, "getInstalledPackages", LogosList::array());
    if (r.value("status", std::string{}) == "error") {
        output().printError(r.value("code", std::string("RPC_FAILED")),
                            r.value("message", std::string{}), r);
        return 1;
    }
    for (const auto& p : r["result"]) {
        if (p.value("name", std::string{}) != target) continue;
        if (output().isJsonMode()) { output().printRaw(p.dump()); return 0; }
        for (const char* k : {"name", "version", "type", "category", "author",
                              "license", "description", "installType", "installDir"}) {
            if (p.contains(k) && p[k].is_string() && !p[k].get<std::string>().empty())
                output().printRaw(fmt::format("{:<14} {}", std::string(k) + ":",
                                              p[k].get<std::string>()));
        }
        const auto& deps = p["dependencies"];
        if (deps.is_array() && !deps.empty()) {
            std::vector<std::string> d;
            for (const auto& e : deps) d.push_back(e.get<std::string>());
            output().printRaw(fmt::format("{:<14} {}", "dependencies:", fmt::join(d, ", ")));
        }
        return 0;
    }
    output().printError("PACKAGE_NOT_INSTALLED", "Package '" + target + "' is not installed.");
    return 1;
}

// ── deps ─────────────────────────────────────────────────────────────────────

int PackageCommand::deps(const std::vector<std::string>& args)
{
    CLI::App cli{"package deps"};
    cli.set_help_flag();
    std::string pkg;
    cli.add_option("name", pkg, "Package name")->required();
    bool recursive = false, reverse = false;
    cli.add_flag("-r,--recursive", recursive, "Walk the graph transitively");
    cli.add_flag("--reverse", reverse, "Show dependents instead of dependencies");
    try { parseArgs(cli, args); }
    catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl package deps <name> [-r] [--reverse]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0) return err;

    const char* method = reverse ? "resolveFlatDependents" : "resolveFlatDependencies";
    LogosMap r = client().callModuleMethod(kPm, method, LogosList{pkg, recursive});
    if (r.value("status", std::string{}) == "error") {
        output().printError(r.value("code", std::string("RPC_FAILED")),
                            r.value("message", std::string{}), r);
        return 1;
    }
    const auto& nodes = r["result"];
    if (output().isJsonMode()) { output().printRaw(nodes.dump()); return 0; }
    if (!nodes.is_array() || nodes.empty()) {
        output().printRaw(reverse ? "Nothing depends on this package."
                                  : "This package has no dependencies.");
        return 0;
    }
    for (const auto& n : nodes) {
        output().printRaw(fmt::format("{:<28} {:<12} {}",
            n.value("name", std::string{}),
            n.value("version", std::string("-")),
            n.value("status", n.value("installType", std::string{}))));
    }
    return 0;
}

// ── search ───────────────────────────────────────────────────────────────────

int PackageCommand::search(const std::vector<std::string>& args)
{
    CLI::App cli{"package search"};
    cli.set_help_flag();
    std::string query, category, catalog;
    cli.add_option("query", query, "Search text (omit to list everything)");
    cli.add_option("--category", category, "Filter by category");
    cli.add_option("--catalog", catalog, "Restrict to one catalog (url or name)");
    try { parseArgs(cli, args); }
    catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl package search [query] [--category C] [--catalog C]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0) return err;

    LogosMap r = catalog.empty()
        ? client().callModuleMethod(kPd, "getCatalog", LogosList::array())
        : client().callModuleMethod(kPd, "getCatalogForRepo", LogosList{catalog});
    if (r.value("status", std::string{}) == "error") {
        output().printError(r.value("code", std::string("RPC_FAILED")),
                            r.value("message", std::string{}), r);
        return 1;
    }

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    const std::string q = lower(query), cat = lower(category);

    LogosList hits = LogosList::array();
    for (const auto& p : r["result"]) {
        if (!cat.empty() && lower(p.value("category", std::string{})) != cat) continue;
        if (!q.empty()) {
            const std::string hay = lower(p.value("name", std::string{}) + " " +
                                          p.value("description", std::string{}) + " " +
                                          p.value("displayName", std::string{}));
            if (hay.find(q) == std::string::npos) continue;
        }
        hits.push_back(p);
    }

    if (output().isJsonMode()) { output().printRaw(hits.dump()); return 0; }
    if (hits.empty()) { output().printRaw("No packages found."); return 0; }

    output().printRaw(fmt::format("{:<24} {:<10} {:<14} {}",
                                  "NAME", "VERSION", "CATEGORY", "DESCRIPTION"));
    for (const auto& p : hits) {
        std::string version = "-";
        const auto& versions = p["versions"];
        if (versions.is_array() && !versions.empty()) {
            const auto& m = versions[0]["manifest"];
            if (m.is_object()) version = m.value("version", std::string("-"));
        }
        std::string desc = p.value("description", std::string{});
        if (desc.size() > 48) desc = desc.substr(0, 45) + "...";
        output().printRaw(fmt::format("{:<24} {:<10} {:<14} {}",
            p.value("name", std::string{}), version,
            p.value("category", std::string("-")), desc));
    }
    return 0;
}

// ── download ─────────────────────────────────────────────────────────────────

int PackageCommand::download(const std::vector<std::string>& args)
{
    CLI::App cli{"package download"};
    cli.set_help_flag();
    std::string pkg, version, rootHash, catalog, outDir;
    cli.add_option("name", pkg, "Package name")->required();
    cli.add_option("--version", version, "Pin an exact version");
    cli.add_option("--root-hash", rootHash, "Disambiguate releases sharing a version");
    cli.add_option("--catalog", catalog, "Restrict to one catalog (url or name)");
    cli.add_option("-o,--output", outDir,
        "Directory to write the .lgx into (on the daemon's host; "
        "defaults to the session's cache/downloads)");
    try { parseArgs(cli, args); }
    catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl package download <name> [--version V] [-o DIR]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0) return err;

    // The daemon does the move, because the file lands on its filesystem, not
    // ours. Resolve a relative -o here so it means what the user typed in
    // *this* shell -- correct for the usual local daemon. Against a remote one
    // the result is an absolute path over there, which fails loudly if it is
    // not writable rather than quietly writing somewhere else.
    if (!outDir.empty()) {
        std::error_code ec;
        const std::filesystem::path p(outDir);
        if (p.is_relative()) {
            const auto abs = std::filesystem::absolute(p, ec);
            if (!ec) outDir = abs.lexically_normal().string();
        }
    }

    LogosMap opts = LogosMap::object();
    opts["version"] = version;
    opts["root_hash"] = rootHash;
    opts["catalog"] = catalog;
    opts["output"] = outDir;

    LogosMap r = client().downloadPackage(pkg, opts);
    if (r.value("status", std::string{}) == "error") {
        output().printError(r.value("code", std::string("RPC_FAILED")),
                            r.value("message", std::string{}), r);
        return 1;
    }
    const auto& res = r["result"];
    const std::string path = res.value("path", std::string{});
    if (path.empty()) {
        output().printError("DOWNLOAD_FAILED", "Could not download '" + pkg + "'.");
        return 1;
    }
    if (output().isJsonMode()) { output().printSuccess(res); return 0; }
    output().printRaw(fmt::format("Downloaded {} -> {}", pkg, path));
    (void)jsonArg;
    return 0;
}
