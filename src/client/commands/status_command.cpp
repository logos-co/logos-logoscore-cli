#include "status_command.h"
#include "../client_state.h"
#include "../../daemon/daemon_state.h"

#include <QJsonObject>

#include <signal.h>
#include <errno.h>

// Two checks before falling through to RPC liveness:
//
//   1. ClientStateFile — if the operator hasn't configured a client
//      (no client/config.json), there's nothing to dial. Distinct
//      from "daemon not running" — the daemon may be up, we just
//      don't have a config pointing at it.
//
//   2. Local daemon/state.json — short-circuit "no live daemon" for
//      the same-host case. If state.json is present but its pid is
//      not alive (`kill(pid, 0) == ESRCH`), the previous daemon
//      hard-crashed and left a stale file behind; we surface that
//      directly instead of timing out an RPC. If state.json is
//      missing, we fall through to RPC — the daemon could still be
//      a remote one whose state.json never appears on our host.
//
// On success past both: try RPC. Any success means the daemon is at
// least reachable enough to answer.

int StatusCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    // No client config ⇒ nothing to dial. (Different from
    // "daemon not running" — the daemon may be up, we just don't
    // have a client config pointing at it.)
    if (!ClientStateFile::read().fileOk) {
        QJsonObject result;
        result["daemon"] = QJsonObject{{"status", "not_configured"}};
        output().printStatus(result);
        return 1;
    }

    // Local-daemon staleness check. State.json on disk + dead pid
    // means the previous daemon crashed before it could clean up.
    // Surface that directly so the operator gets a fast, accurate
    // signal instead of waiting on an RPC timeout. Missing state.json
    // is *not* an error — it could mean a remote daemon (no state on
    // our host) or no daemon at all (RPC will confirm).
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
