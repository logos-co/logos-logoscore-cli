#include "stop_command.h"

int StopCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    int err = ensureConnected();
    if (err != 0)
        return err;

    QJsonObject result = client().shutdown();

    std::string status = result.value("status").toString().toStdString();

    // If the daemon shut down before the RPC response arrived, the call
    // returns an RPC_FAILED error. That's expected — treat it as success.
    if (status == "error") {
        std::string code = result.value("code").toString().toStdString();
        if (code == "RPC_FAILED") {
            if (output().isJsonMode()) {
                QJsonObject ok;
                ok["status"]  = "ok";
                ok["message"] = "Daemon stopped.";
                output().printSuccess(ok);
            } else {
                output().printRaw("Daemon stopped.");
            }
            return 0;
        }
        output().printError(code, result.value("message").toString().toStdString());
        return 1;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw("Daemon stopped.");
    }
    return 0;
}
