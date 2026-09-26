#include "peer_command.h"
#include "../remote.h"
#include "../../string_utils.h"

#include <fmt/format.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace {

constexpr const char* kUsage =
    "Usage: logosctl peer <verb> ...\n"
    "  status | ls | pending | routes | exports | imports\n"
    "  pair-window SECONDS           admit code pairing for a while (0 closes)\n"
    "  pair HOST PORT                start pairing; compare the code, then accept\n"
    "  accept ID [--allow M,N] | reject ID\n"
    "                                decide a pending pairing; --allow lets the peer call M,N here\n"
    "  invite [--operator] [--ttl S] [--allow M,N]\n"
    "                                print a single-use invite; its redeemer may call M,N here\n"
    "  redeem [FILE|-] [--allow M,N] redeem an invite read from FILE or stdin\n"
    "  remove PEER | rename PEER ALIAS\n"
    "  export MODULE [--events] | unexport MODULE\n"
    "  import NAME --from PEER [--module M] [--allow A,B] [--events] [--prefer remote|local]\n"
    "  unimport NAME\n"
    "  policy [show] | policy set FILE";

bool readAll(std::istream& in, std::string& out)
{
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

std::optional<long long> number(const std::string& text)
{
    try {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used);
        if (used == text.size()) return value;
    } catch (...) {
    }
    return std::nullopt;
}

std::vector<std::string> splitComma(const std::string& text)
{
    std::vector<std::string> out;
    std::stringstream in(text);
    for (std::string item; std::getline(in, item, ',');)
        if (!item.empty()) out.push_back(item);
    return out;
}

} // namespace

std::optional<LogosMap> PeerCommand::call(const std::string& method, const LogosList& args)
{
    LogosMap result = client().callModuleMethod("peering_module", method, args);
    if (result.value("status", std::string{}) == "error") {
        output().printError(result.value("code", std::string{}),
                            result.value("message", std::string{}), result);
        return std::nullopt;
    }
    LogosMap answer = result.contains("result") ? result["result"] : LogosMap::object();
    if (answer.is_object() && answer.contains("error")) {
        const std::string why = answer["error"].is_string() ? answer["error"].get<std::string>() : "failed";
        output().printError("PEERING_REFUSED", why, answer);
        return std::nullopt;
    }
    return answer;
}

std::optional<std::string> PeerCommand::resolvePeer(const std::string& peer)
{
    const auto peers = call("peers", LogosList::array());
    if (!peers) return std::nullopt;
    for (const auto& p : peers->value("peers", LogosList::array()))
        if (p.value("runtime_id", "") == peer || p.value("alias", "") == peer)
            return p.value("runtime_id", "");
    output().printError("NO_SUCH_PEER", fmt::format("No paired runtime is called {}", peer));
    return std::nullopt;
}

int PeerCommand::print(const LogosMap& result)
{
    if (output().isJsonMode()) output().printSuccess(result);
    else output().printRaw(result.dump(2));
    return 0;
}

int PeerCommand::execute(const std::vector<std::string>& args)
{
    if (args.empty()) {
        output().printError("INVALID_ARGS", kUsage);
        return 1;
    }
    const std::string verb = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    const auto usage = [&] {
        output().printError("INVALID_ARGS", kUsage);
        return 1;
    };

    if (int err = ensureConnected(); err != 0) return err;

    const auto simple = [&](const std::string& method, const LogosList& callArgs) {
        const auto result = call(method, callArgs);
        return result ? print(*result) : 4;
    };

    if (verb == "status" && rest.empty()) return simple("status", LogosList::array());
    if ((verb == "ls" || verb == "peers") && rest.empty()) return simple("peers", LogosList::array());
    if (verb == "pending" && rest.empty()) return simple("pending", LogosList::array());
    if (verb == "routes" && rest.empty()) return simple("routes", LogosList::array());
    if (verb == "exports" && rest.empty()) return simple("exports", LogosList::array());
    if (verb == "imports" && rest.empty()) return simple("imports", LogosList::array());
    if (verb == "pair-window" && rest.size() == 1 && number(rest[0]))
        return simple("openPairingWindow", LogosList::array({*number(rest[0])}));
    if ((verb == "accept" || verb == "reject") && rest.size() == 1)
        return simple(verb == "accept" ? "confirmPairing" : "rejectPairing", LogosList::array({rest[0]}));
    if (verb == "accept" && rest.size() == 3 && rest[1] == "--allow")
        return simple("confirmPairing", LogosList::array({rest[0], splitComma(rest[2])}));
    if (verb == "remove" && rest.size() == 1) return simple("removePeer", LogosList::array({rest[0]}));
    if (verb == "rename" && rest.size() == 2)
        return simple("renamePeer", LogosList::array({rest[0], rest[1]}));
    if (verb == "unexport" && rest.size() == 1) return simple("removeExport", LogosList::array({rest[0]}));
    if (verb == "unimport" && rest.size() == 1) return simple("removeImport", LogosList::array({rest[0]}));

    if (verb == "pair" && rest.size() == 2 && number(rest[1])) {
        const auto result = call("pairWith", LogosList::array({rest[0], *number(rest[1])}));
        if (!result) return 4;
        if (output().isJsonMode()) return print(*result);
        output().printRaw(fmt::format(
            "Pairing code {} with {}.\nIf the other screen shows the same code: logosctl peer accept {}",
            result->value("code", ""), result->value("peer_display_id", ""), result->value("id", "")));
        return 0;
    }

    if (verb == "invite") {
        std::string role = "peer";
        long long ttl = 0;
        LogosList allow;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == "--operator") role = "operator";
            else if (rest[i] == "--ttl" && i + 1 < rest.size() && number(rest[i + 1])) ttl = *number(rest[++i]);
            else if (rest[i] == "--allow" && i + 1 < rest.size()) allow = splitComma(rest[++i]);
            else return usage();
        }
        const auto result = call("createInvite", LogosList::array({role, ttl, allow}));
        if (!result) return 4;
        if (output().isJsonMode()) return print(*result);
        output().printRaw(result->value("invite", ""));
        return 0;
    }

    // Invites are secrets: read from a file or stdin, never taken from argv.
    if (verb == "redeem") {
        std::string source = "-";
        bool sourceGiven = false;
        LogosList allow;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            if (rest[i] == "--allow" && i + 1 < rest.size()) allow = splitComma(rest[++i]);
            else if (!sourceGiven) { source = rest[i]; sourceGiven = true; }
            else return usage();
        }
        std::string text;
        bool read = false;
        if (source == "-") {
            read = readAll(std::cin, text);
        } else {
            std::ifstream file(source);
            read = file.is_open() && readAll(file, text);
        }
        text = logosctl::remote::inviteText(strutil::trim(text));
        if (!read || text.empty()) {
            output().printError("INVALID_ARGS", "No invite to redeem");
            return 1;
        }
        return simple("redeemInvite", LogosList::array({text, allow}));
    }

    if (verb == "export" && !rest.empty()) {
        bool events = false;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            if (rest[i] == "--events") events = true;
            else return usage();
        }
        return simple("setExport", LogosList::array({rest[0], LogosMap{{"events", events}}}));
    }

    if (verb == "import" && !rest.empty()) {
        LogosMap rule = LogosMap::object();
        std::string from;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            const std::string& flag = rest[i];
            if (flag == "--events") {
                rule["events"] = true;
            } else if (i + 1 < rest.size() && flag == "--from") {
                from = rest[++i];
            } else if (i + 1 < rest.size() && flag == "--module") {
                rule["module"] = rest[++i];
            } else if (i + 1 < rest.size() && flag == "--allow") {
                rule["allowed_callers"] = splitComma(rest[++i]);
            } else if (i + 1 < rest.size() && flag == "--prefer") {
                rule["prefer"] = rest[++i];
            } else {
                return usage();
            }
        }
        if (from.empty()) return usage();
        const auto runtimeId = resolvePeer(from);
        if (!runtimeId) return 4;
        rule["from"] = *runtimeId;
        return simple("setImport", LogosList::array({rest[0], rule}));
    }

    if (verb == "policy" && (rest.empty() || (rest.size() == 1 && rest[0] == "show")))
        return simple("remotePolicy", LogosList::array());
    if (verb == "policy" && rest.size() == 2 && rest[0] == "set") {
        std::ifstream file(rest[1]);
        std::string text;
        if (!file.is_open() || !readAll(file, text)) {
            output().printError("INVALID_ARGS", "Cannot read " + rest[1]);
            return 1;
        }
        const LogosMap policy = LogosMap::parse(text, nullptr, false);
        if (!policy.is_object()) {
            output().printError("INVALID_ARGS", rest[1] + " is not a JSON object");
            return 1;
        }
        return simple("setPolicy", LogosList::array({policy}));
    }

    return usage();
}
