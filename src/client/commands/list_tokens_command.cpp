#include "list_tokens_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <fmt/format.h>

int ListTokensCommand::execute(const std::vector<std::string>& /*args*/)
{
    TokenStore store;
    const auto issued = store.listTokens();

    LogosList arr = LogosList::array();
    for (const auto& t : issued) {
        LogosMap o;
        o["name"]             = t.name;
        o["issued_at"]        = t.issuedAt;
        o["expires_at"]       = t.expiresAt.empty() ? nullptr : nlohmann::json(t.expiresAt);
        o["local_only"]       = t.localOnly;
        o["raw_file_present"] = t.rawFilePresent;
        arr.push_back(o);
    }

    if (output().isJsonMode()) {
        output().printSuccess(arr);
    } else if (issued.empty()) {
        output().printRaw("No tokens issued.");
    } else {
        output().printRaw(fmt::format("{} token(s):", issued.size()));
        for (const auto& t : issued) {
            std::string line = fmt::format("  {}  (issued {}", t.name, t.issuedAt);
            if (!t.expiresAt.empty())
                line += fmt::format(", expires {}", t.expiresAt);
            if (t.localOnly)
                line += ", local_only";
            line += ")";
            if (!t.rawFilePresent)
                line += "  [raw file deleted]";
            output().printRaw(line);
        }
    }
    return 0;
}
