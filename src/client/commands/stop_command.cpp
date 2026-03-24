#include "stop_command.h"

int StopCommand::execute(const QStringList& args)
{
    Q_UNUSED(args);

    int err = ensureConnected();
    if (err != 0)
        return err;

    QJsonObject result = client().shutdown();

    if (result.value("status").toString() == "error") {
        output().printError(result.value("code").toString(),
                           result.value("message").toString());
        return 1;
    }

    output().printSuccess(result);
    return 0;
}
