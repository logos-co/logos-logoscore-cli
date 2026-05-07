#include "list_tokens_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

int ListTokensCommand::execute(const std::vector<std::string>& /*args*/)
{
    TokenStore store;
    const auto issued = store.listTokens();

    QJsonArray arr;
    for (const auto& t : issued) {
        QJsonObject o;
        o["name"]            = QString::fromStdString(t.name);
        o["issued_at"]       = QString::fromStdString(t.issuedAt);
        o["expires_at"]      = t.expiresAt.empty()
                             ? QJsonValue(QJsonValue::Null)
                             : QJsonValue(QString::fromStdString(t.expiresAt));
        o["local_only"]      = t.localOnly;
        o["raw_file_present"] = t.rawFilePresent;
        arr.append(o);
    }

    if (output().isJsonMode()) {
        output().printRaw(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    } else if (issued.empty()) {
        output().printRaw("No tokens issued.");
    } else {
        output().printRaw(QString("%1 token(s):").arg(issued.size()));
        for (const auto& t : issued) {
            QString line = QString("  %1  (issued %2")
                .arg(QString::fromStdString(t.name),
                     QString::fromStdString(t.issuedAt));
            if (!t.expiresAt.empty())
                line += QString(", expires %1").arg(QString::fromStdString(t.expiresAt));
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
