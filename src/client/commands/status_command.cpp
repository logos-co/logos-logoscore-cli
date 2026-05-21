#include "status_command.h"
#include "../client_state.h"
#include "../../daemon/daemon_state.h"

#include <QJsonObject>

#include <signal.h>
#include <errno.h>

int StatusCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    if (!ClientStateFile::read().fileOk) {
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_configured"}};
        output().printStatus(result);
        return 1;
    }

    {
        const DaemonRuntimeState rs = DaemonRuntimeStateFile::read();
        if (rs.fileOk && rs.pid > 0 && ::kill(static_cast<pid_t>(rs.pid), 0) != 0
                                    && errno == ESRCH) {
            QJsonObject result;
            result["daemon"] = QJsonObject{
                {"status", "not_running"},
                {"reason", "stale state file (daemon crashed; pid no longer alive)"},
                {"pid",    qlonglong(rs.pid)},
            };
            output().printStatus(result);
            return 1;
        }
    }

    int err = ensureConnected();
    if (err != 0) {
        QJsonObject result;
        result["daemon"] = QJsonObject{
            {"status", "not_running"},
            {"reason", QString::fromStdString(client().lastError())},
        };
        output().printStatus(result);
        return 1;
    }

    QJsonObject status = client().getStatus();

    if (status.contains("status") &&
        status.value("status").toString().toStdString() == "error") {
        output().printError(status.value("code").toString().toStdString(),
                            status.value("message").toString().toStdString());
        return 1;
    }

    if (!status.contains("daemon")) {
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_running"}};
        output().printStatus(result);
        return 1;
    }

    output().printStatus(status);
    return 0;
}
