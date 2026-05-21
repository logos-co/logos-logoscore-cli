#include "revoke_token_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <QJsonObject>
#include <fmt/format.h>

int RevokeTokenCommand::execute(const std::vector<std::string>& args)
{
    if (args.empty()) {
        output().printError("INVALID_ARGS", "Usage: logoscore revoke-token NAME");
        return 1;
    }
    const std::string name = args[0];

    TokenStore store;
    switch (store.revokeToken(name)) {
        case TokenStore::RevokeStatus::Ok:
            break;
        case TokenStore::RevokeStatus::InvalidName:
            output().printError("INVALID_NAME",
                fmt::format("Token name '{}' is invalid (must be 1-64 chars, "
                            "alnum/dot/dash/underscore, no traversal).", name));
            return 1;
        case TokenStore::RevokeStatus::NotFound:
            output().printError("TOKEN_NOT_FOUND",
                fmt::format("No token named '{}' exists.", name));
            return 3;
        case TokenStore::RevokeStatus::IoError:
            output().printError("IO_ERROR",
                fmt::format("Failed to update tokens.json while revoking '{}'. "
                            "Check permissions/free space under {}.",
                            name, Config::daemonTokensDir()));
            return 1;
    }

    QJsonObject result;
    result["status"] = "ok";
    result["name"]   = QString::fromStdString(name);
    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(fmt::format("Revoked token: {}", name));
    }
    return 0;
}
