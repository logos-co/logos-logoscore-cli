#include "list_tokens_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

int ListTokensCommand::execute(const std::vector<std::string>& /*args*/)
{
    TokenStore store(Config::configDir().toStdString());
    const auto issued = store.listTokens();

    QJsonArray arr;
    for (const auto& t : issued) {
        QJsonObject o;
        o["name"]      = QString::fromStdString(t.name);
        o["issued_at"] = QString::fromStdString(t.issuedAt);
        arr.append(o);
    }

    if (output().isJsonMode()) {
        output().printRaw(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    } else if (issued.empty()) {
        output().printRaw("No tokens issued.");
    } else {
        output().printRaw(QString("%1 token(s):").arg(issued.size()));
        for (const auto& t : issued) {
            output().printRaw(QString("  %1  (issued %2)")
                .arg(QString::fromStdString(t.name),
                     QString::fromStdString(t.issuedAt)));
        }
    }
    return 0;
}
