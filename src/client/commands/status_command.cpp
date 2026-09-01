#include "status_command.h"
#include "../client_state.h"

int StatusCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    if (!ClientStateFile::read().fileOk) {
        LogosMap result{{"daemon", LogosMap{{"status","not_configured"}}}};
        output().printStatus(result);
        return 1;
    }

    // The same guard ensureConnected() applies, one step earlier and reported
    // differently: "no daemon" is an answer to `status`, not an error, so this
    // prints a status report and exits 1 rather than NO_DAEMON and 2.
    if (const std::optional<StaleSession> stale = detectStaleSession()) {
        LogosMap result{{"daemon", LogosMap{
            {"status", "not_running"},
            {"reason", stale->reason},
            {"pid",    stale->pid},
        }}};
        output().printStatus(result);
        return 1;
    }

    // Connect directly rather than through ensureConnected(): that helper
    // PRINTS a NO_DAEMON error envelope on failure, and this command's answer
    // to "no daemon" is a status report. Going through it put two JSON
    // documents on stdout for one command, which no `jq` invocation survives.
    if (!client().isConnected() && !client().connect()) {
        LogosMap result{{"daemon", LogosMap{
            {"status", "not_running"},
            {"reason", client().lastError()},
        }}};
        output().printStatus(result);
        return 1;
    }

    LogosMap status = client().getStatus();

    if (status.value("status", std::string{}) == "error") {
        output().printError(status.value("code", std::string{}),
                            status.value("message", std::string{}));
        return 1;
    }

    if (!status.contains("daemon")) {
        LogosMap result{{"daemon", LogosMap{{"status","not_running"}}}};
        output().printStatus(result);
        return 1;
    }

    output().printStatus(status);

    // RpcClient::getStatus synthesises a `not_running` report when the RPC
    // produced no reply, and marks it with `rpc_error`. That report has a
    // "daemon" key, so it used to reach the success branch here: `status`
    // printed "not_running" and exited 0. Exit 0 reads as "the daemon is fine"
    // to anything checking the code rather than the text, and docs/project.md
    // has always promised 1 for a daemon that is not running.
    return status.contains("rpc_error") ? 1 : 0;
}
