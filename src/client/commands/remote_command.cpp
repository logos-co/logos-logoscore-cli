#include "remote_command.h"
#include "../remote.h"
#include "../../string_utils.h"

#include <fmt/format.h>

#include <fstream>
#include <iostream>
#include <iterator>

namespace {

constexpr const char* kUsage =
    "Usage: logosctl remote <verb> ...\n"
    "  pair [FILE|-]    redeem a daemon's operator invite (`logosctl peer invite --operator`\n"
    "                   there), read from FILE or stdin; waits until the daemon accepts it\n"
    "  ls               this client's paired daemons, and its own ID\n"
    "  remove PEER      forget a daemon\n"
    "Then: logosctl --remote PEER <command>";

// How long a pairing may wait for the daemon's operator to accept it.
constexpr std::chrono::seconds kPairingWait{300};

} // namespace

int RemoteCommand::execute(const std::vector<std::string>& args)
{
    const std::string verb = args.empty() ? "" : args[0];
    const std::vector<std::string> rest(args.begin() + (args.empty() ? 0 : 1), args.end());
    std::string error;

    if (verb == "ls" && rest.empty()) {
        const auto me = logosctl::remote::self(&error);
        const auto peers = me ? logosctl::remote::peers(&error) : std::nullopt;
        if (!peers) {
            output().printError("REMOTE_UNAVAILABLE", error);
            return 1;
        }
        const LogosMap result{{"self", *me}, {"peers", *peers}};
        if (output().isJsonMode()) output().printSuccess(result);
        else output().printRaw(result.dump(2));
        return 0;
    }

    // Invites are secrets: read from a file or stdin, never taken from argv.
    if (verb == "pair" && rest.size() <= 1) {
        std::string text;
        if (rest.empty() || rest[0] == "-") {
            text.assign(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
        } else {
            std::ifstream file(rest[0]);
            text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        text = logosctl::remote::inviteText(strutil::trim(text));
        if (text.empty()) {
            output().printError("INVALID_ARGS", "No invite to redeem");
            return 1;
        }
        const auto me = logosctl::remote::self(&error);
        if (!me) {
            output().printError("REMOTE_UNAVAILABLE", error);
            return 1;
        }
        const auto paired = logosctl::remote::pair(text, kPairingWait, [&](const LogosMap& state) {
            if (state.value("state", "") == "confirming" && !output().isJsonMode())
                std::cerr << fmt::format("Waiting for {} to accept this client, ID {} "
                                         "(there: logosctl peer pending, then peer accept ID)\n",
                                         state.value("peer_display_id", std::string("the daemon")),
                                         me->value("display_id", ""));
        }, &error);
        if (!paired) {
            output().printError("PAIRING_FAILED", error);
            return 4;
        }
        if (output().isJsonMode()) output().printSuccess(*paired);
        else output().printSuccess(fmt::format("Paired with {} ({}). Use: logosctl --remote {} status",
                                               paired->value("alias", ""),
                                               paired->value("display_id", ""),
                                               paired->value("alias", "")));
        return 0;
    }

    if (verb == "remove" && rest.size() == 1) {
        if (!logosctl::remote::remove(rest[0], &error)) {
            output().printError("REMOTE_UNAVAILABLE", error);
            return 4;
        }
        output().printSuccess("Removed " + rest[0] + ". The daemon still lists this client until "
                              "its operator removes it there.");
        return 0;
    }

    output().printError("INVALID_ARGS", kUsage);
    return 1;
}
