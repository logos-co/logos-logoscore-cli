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

    TokenStore store(Config::configDir().toStdString());
    if (!store.revokeToken(name)) {
        output().printError("TOKEN_NOT_FOUND",
            QString("No token named '%1' exists.").arg(QString::fromStdString(name)));
        return 3;
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
