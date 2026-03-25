#include "load_module_command.h"

int LoadModuleCommand::execute(const QStringList& args)
{
    if (args.isEmpty()) {
        output().printError("INVALID_ARGS", "Usage: logoscore load-module <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    QString moduleName = args.first();
    QJsonObject result = client().loadModule(moduleName);

    QString status = result.value("status").toString();
    if (status == "error") {
        QString code = result.value("code").toString();
        output().printError(code, result.value("message").toString(), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        QString version = result.value("version").toString();
        QJsonArray deps = result.value("dependencies_loaded").toArray();

        output().printRaw(QString("Loaded module: %1 (v%2)").arg(moduleName, version));
        if (!deps.isEmpty()) {
            QStringList depNames;
            for (const QJsonValue& v : deps)
                depNames.append(v.toString());
            output().printRaw(QString("  Dependencies loaded: %1").arg(depNames.join(", ")));
        }
    }

    return 0;
}
