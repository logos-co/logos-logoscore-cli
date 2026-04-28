#include "status_command.h"
#include "../client_state.h"

#include <QJsonObject>

// No separate liveness probe. The status command IS the liveness check:
// if `getStatus` fails (RPC timeout, connection refused, daemon not
// listening on the endpoint we picked after --client-tcp-* overrides…),
// we report `not_running`. Any success means the daemon is at least
// reachable enough to answer.

int StatusCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    // No client config ⇒ nothing to dial. (Different from
    // "daemon not running" — the daemon may be up, we just don't
    // have a client.json pointing at it.)
    if (!ClientStateFile::read().fileOk) {
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_configured"}};
        output().printStatus(result);
        return 1;
    }

    // Try to connect and ask the daemon directly. Both failure modes
    // (can't set up client / RPC itself fails) mean "not running" as
    // far as the user cares.
    int err = ensureConnected();
    if (err != 0) {
        QJsonObject result;
        result["daemon"] = QJsonObject{
            {"status", "not_running"},
            {"reason", client().lastError()},
        };
        output().printStatus(result);
        return 1;
    }

    QJsonObject status = client().getStatus();

    if (status.contains("status") && status.value("status").toString() == "error") {
        output().printError(status.value("code").toString(),
                           status.value("message").toString());
        return 1;
    }

    // Empty / missing "daemon" key means the RPC returned nothing
    // useful — treat as "not running" so the CLI exits non-zero
    // instead of silently claiming success on an unreachable daemon.
    if (!status.contains("daemon")) {
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_running"}};
        output().printStatus(result);
        return 1;
    }

    output().printStatus(status);
    return 0;
}
