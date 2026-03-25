#include "module_info_command.h"

int ModuleInfoCommand::execute(const QStringList& args)
{
    if (args.isEmpty()) {
        output().printError("INVALID_ARGS", "Usage: logoscore module-info <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    QString moduleName = args.first();
    QJsonObject info = client().getModuleInfo(moduleName);

    QString status = info.value("status").toString();
    if (status == "error") {
        output().printError(info.value("code").toString(),
                           info.value("message").toString(), info);
        return 3;
    }

    output().printModuleInfo(info);
    return 0;
}
