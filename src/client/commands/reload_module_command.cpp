#include "reload_module_command.h"

int ReloadModuleCommand::execute(const QStringList& args)
{
    if (args.isEmpty()) {
        output().printError("INVALID_ARGS", "Usage: logoscore reload-module <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    QString moduleName = args.first();

    if (!output().isJsonMode()) {
        // Print progress to stderr in human mode
        fprintf(stderr, "Reloading %s...\n", moduleName.toUtf8().constData());
    }

    QJsonObject result = client().reloadModule(moduleName);

    QString status = result.value("status").toString();
    if (status == "error") {
        output().printError(result.value("code").toString(),
                           result.value("message").toString(), result);
        return 3;
    }

    output().printReload(result);
    return 0;
}
