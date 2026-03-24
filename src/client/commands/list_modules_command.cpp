#include "list_modules_command.h"

int ListModulesCommand::execute(const QStringList& args)
{
    int err = ensureConnected();
    if (err != 0)
        return err;

    QString filter = "all";
    if (args.contains("--loaded"))
        filter = "loaded";

    QJsonArray modules = client().listModules(filter);

    output().printModuleList(modules);
    return 0;
}
