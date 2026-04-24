#include "issue_token_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <CLI/CLI.hpp>

#include <QJsonObject>
#include <QString>

int IssueTokenCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"issue-token"};
    cli.set_help_flag();
    std::string name;
    bool replace = false;
    cli.add_option("--name", name, "Name for this client token")->required();
    cli.add_flag("--replace", replace, "Replace an existing token with this name");

    try {
        std::vector<std::string> argsCopy(args.rbegin(), args.rend());
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore issue-token --name NAME [--replace]");
        return 1;
    }

    TokenStore store(Config::configDir().toStdString());
    auto maybeToken = store.issueToken(name, replace);
    if (!maybeToken) {
        output().printError("TOKEN_EXISTS",
            QString("Token '%1' already exists. Use --replace to overwrite.")
                .arg(QString::fromStdString(name)));
        return 3;
    }

    // Write the convenience file so the client can `--token-file ...`.
    if (!store.writeClientFile(name, *maybeToken, "daemon.json")) {
        output().printError("TOKEN_WRITE_FAILED",
            "Issued token but failed to write the client file.");
        return 1;
    }

    QJsonObject result;
    result["status"]   = "ok";
    result["name"]     = QString::fromStdString(name);
    result["token"]    = QString::fromStdString(*maybeToken);
    result["file"]     = QString::fromStdString(store.clientFilePath(name));

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(QString("Issued token for '%1'").arg(QString::fromStdString(name)));
        output().printRaw(QString("  file:  %1").arg(QString::fromStdString(store.clientFilePath(name))));
        output().printRaw(QString("  token: %1").arg(QString::fromStdString(*maybeToken)));
    }
    return 0;
}
