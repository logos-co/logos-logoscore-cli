#include "status_command.h"
#include "../../daemon/connection_file.h"

int StatusCommand::execute(const QStringList& args)
{
    Q_UNUSED(args);

    // First check if connection file exists and PID is alive
    ConnectionInfo connInfo = ConnectionFile::read();

    if (!connInfo.valid) {
        // Daemon not running - report status, exit 1
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_running"}};
        output().printStatus(result);
        return 1;
    }

    // Connect to daemon
    int err = ensureConnected();
    if (err != 0) {
        // If we can't connect but PID was alive, still report not_running
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_running"}};
        output().printStatus(result);
        return 1;
    }

    QJsonObject status = client().getStatus();

    if (status.contains("status") && status.value("status").toString() == "error") {
        output().printError(status.value("code").toString(),
                           status.value("message").toString());
        return 1;
    }

    output().printStatus(status);
    return 0;
}
