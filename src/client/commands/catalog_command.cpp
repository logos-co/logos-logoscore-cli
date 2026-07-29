#include "catalog_command.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace {

constexpr const char* kPd = "package_downloader";
constexpr const char* kPm = "package_manager";

} // namespace

int CatalogCommand::execute(const std::vector<std::string>& args)
{
    if (args.empty()) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl catalog <ls|add|remove|enable|disable|refresh> [url]");
        return 1;
    }

    const std::string sub = args[0];
    const std::string url = args.size() > 1 ? args[1] : std::string{};

    const bool needsUrl = (sub == "add" || sub == "remove" ||
                           sub == "enable" || sub == "disable");
    if (needsUrl && url.empty()) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl catalog " + sub + " <url>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0) return err;

    if (sub == "ls" || sub == "list") {
        LogosMap r = client().callModuleMethod(kPd, "listRepositories", LogosList::array());
        if (r.value("status", std::string{}) == "error") {
            output().printError(r.value("code", std::string("RPC_FAILED")),
                                r.value("message", std::string{}), r);
            return 1;
        }
        const auto& repos = r["result"];
        if (output().isJsonMode()) { output().printRaw(repos.dump()); return 0; }
        if (!repos.is_array() || repos.empty()) {
            output().printRaw("No catalogs configured.");
            return 0;
        }
        for (const auto& c : repos) {
            const bool enabled = c.value("enabled", false);
            const std::string resolveError = c.value("resolveError", std::string{});
            output().printRaw(fmt::format("{} {:<26} {}{}",
                enabled ? "[on ]" : "[off]",
                c.value("name", std::string("<unresolved>")),
                c.value("url", std::string{}),
                c.value("isDefault", false) ? "  (default)" : ""));
            // A catalog that failed to resolve still lists, but silently
            // returning zero packages from it would be baffling.
            if (!resolveError.empty())
                output().printRaw(fmt::format("      error: {}", resolveError));
        }
        return 0;
    }

    std::string method;
    LogosList callArgs = LogosList::array();
    if (sub == "add")           { method = "addRepository";       callArgs = LogosList{url}; }
    else if (sub == "remove")   { method = "removeRepository";    callArgs = LogosList{url}; }
    else if (sub == "enable")   { method = "setRepositoryEnabled"; callArgs = LogosList{url, true}; }
    else if (sub == "disable")  { method = "setRepositoryEnabled"; callArgs = LogosList{url, false}; }
    else if (sub == "refresh")  { method = "refreshCatalog"; }
    else {
        output().printError("INVALID_ARGS", "Unknown catalog subcommand: " + sub);
        return 1;
    }

    LogosMap r = client().callModuleMethod(kPd, method, callArgs);
    if (r.value("status", std::string{}) == "error") {
        output().printError(r.value("code", std::string("RPC_FAILED")),
                            r.value("message", std::string{}), r);
        return 1;
    }
    const auto& res = r["result"];
    const std::string errMsg = res.is_object() ? res.value("error", std::string{})
                                               : std::string{};
    if (res.is_object() && !res.value("success", true)) {
        output().printError("CATALOG_FAILED",
                            errMsg.empty() ? ("catalog " + sub + " failed") : errMsg);
        return 1;
    }
    if (output().isJsonMode()) { output().printSuccess(res.is_object() ? res : LogosMap{}); return 0; }
    output().printRaw(sub == "refresh" ? "Catalogs refreshed."
                                       : fmt::format("Catalog {}d: {}", sub, url));
    return 0;
}

int KeyCommand::execute(const std::vector<std::string>& args)
{
    if (args.empty()) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl key <ls|add|remove> ...");
        return 1;
    }

    const std::string sub = args[0];
    int err = ensureConnected();
    if (err != 0) return err;

    if (sub == "ls" || sub == "list") {
        LogosMap r = client().callModuleMethod(kPm, "listTrustedKeys", LogosList::array());
        if (r.value("status", std::string{}) == "error") {
            output().printError(r.value("code", std::string("RPC_FAILED")),
                                r.value("message", std::string{}), r);
            return 1;
        }
        const auto& keys = r["result"];
        if (output().isJsonMode()) { output().printRaw(keys.dump()); return 0; }
        if (!keys.is_array() || keys.empty()) {
            output().printRaw("No trusted keys. Packages from unknown signers "
                              "are accepted or rejected per this session's "
                              "signature policy.");
            return 0;
        }
        for (const auto& k : keys)
            output().printRaw(fmt::format("{:<20} {}", k.value("name", std::string{}),
                                          k.value("did", std::string{})));
        return 0;
    }

    if (sub == "add") {
        // name + --did are both required; the display name and url are
        // self-asserted metadata the signer supplies, kept for provenance.
        std::string keyName, did, displayName, url;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--did" && i + 1 < args.size())                did = args[++i];
            else if (args[i] == "--display-name" && i + 1 < args.size())  displayName = args[++i];
            else if (args[i] == "--url" && i + 1 < args.size())           url = args[++i];
            else if (keyName.empty())                                     keyName = args[i];
        }
        if (keyName.empty() || did.empty()) {
            output().printError("INVALID_ARGS",
                "Usage: logosctl key add <name> --did <did:jwk:...> "
                "[--display-name N] [--url U]");
            return 1;
        }
        LogosMap r = client().callModuleMethod(kPm, "addTrustedKey",
                        LogosList{keyName, did, displayName, url});
        if (r.value("status", std::string{}) == "error") {
            output().printError(r.value("code", std::string("RPC_FAILED")),
                                r.value("message", std::string{}), r);
            return 1;
        }
        const auto& res = r["result"];
        if (res.is_object() && !res.value("success", true)) {
            output().printError("KEY_ADD_FAILED", res.value("error", std::string{}));
            return 1;
        }
        if (output().isJsonMode()) { output().printSuccess(res.is_object() ? res : LogosMap{}); return 0; }
        output().printRaw(fmt::format("Trusted key added: {}", keyName));
        return 0;
    }

    if (sub == "remove") {
        if (args.size() < 2) {
            output().printError("INVALID_ARGS", "Usage: logosctl key remove <name>");
            return 1;
        }
        LogosMap r = client().callModuleMethod(kPm, "removeTrustedKey", LogosList{args[1]});
        if (r.value("status", std::string{}) == "error") {
            output().printError(r.value("code", std::string("RPC_FAILED")),
                                r.value("message", std::string{}), r);
            return 1;
        }
        const auto& res = r["result"];
        if (res.is_object() && !res.value("success", true)) {
            output().printError("KEY_REMOVE_FAILED", res.value("error", std::string{}));
            return 1;
        }
        if (output().isJsonMode()) { output().printSuccess(res.is_object() ? res : LogosMap{}); return 0; }
        output().printRaw(fmt::format("Trusted key removed: {}", args[1]));
        return 0;
    }

    output().printError("INVALID_ARGS", "Unknown key subcommand: " + sub);
    return 1;
}
