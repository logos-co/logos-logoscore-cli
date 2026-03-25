#include "unload_module_command.h"

int UnloadModuleCommand::execute(const QStringList& args)
{
    if (args.isEmpty()) {
        output().printError("INVALID_ARGS", "Usage: logoscore unload-module <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    QString moduleName = args.first();
    QJsonObject result = client().unloadModule(moduleName);

    QString status = result.value("status").toString();
    if (status == "error") {
        output().printError(result.value("code").toString(),
                           result.value("message").toString(), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(QString("Unloaded module: %1").arg(moduleName));
    }

    return 0;
}
