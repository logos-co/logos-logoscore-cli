#include "revoke_token_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <QJsonObject>
#include <QString>

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
                QString("Token name '%1' is invalid (must be 1-64 chars, "
                        "alnum/dot/dash/underscore, no traversal).")
                    .arg(QString::fromStdString(name)));
            return 1;
        case TokenStore::RevokeStatus::NotFound:
            output().printError("TOKEN_NOT_FOUND",
                QString("No token named '%1' exists.").arg(QString::fromStdString(name)));
            return 3;
        case TokenStore::RevokeStatus::IoError:
            output().printError("IO_ERROR",
                QString("Failed to update tokens.json while revoking '%1'. "
                        "Check permissions/free space under %2.")
                    .arg(QString::fromStdString(name),
                         Config::daemonTokensDir()));
            return 1;
    }

    QJsonObject result;
    result["status"] = "ok";
    result["name"]   = QString::fromStdString(name);
    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(QString("Revoked token: %1").arg(QString::fromStdString(name)));
    }
    return 0;
}
