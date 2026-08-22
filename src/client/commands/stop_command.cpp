#include "stop_command.h"
#include "../client_state.h"
#include "../../daemon/daemon_state.h"
#include "../../process_util.h"
#include <fmt/format.h>

int StopCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    // A session whose daemon/state.json names a pid that is no longer alive
    // has no daemon to stop -- and the dial spec sitting next to it will still
    // "connect", because a LocalSocket client never verifies that anyone is
    // listening. Left alone, the shutdown RPC then times out and the
    // confirmation behind it ("is the pid from state.json gone?") answers yes
    // about a daemon that was already gone before we said anything, turning a
    // stale session directory into a reported success. StatusCommand applies
    // the same guard, for the same reason.
    //
    // Only when the state file describes the daemon this client would dial: a
    // remote client can have a co-resident daemon's leftovers in its session,
    // and those must not stop it from stopping the daemon it is talking to.
    {
        const DaemonRuntimeState rs = DaemonRuntimeStateFile::read();
        const ClientState        cs = ClientStateFile::read();
        if (rs.fileOk && rs.pid > 0
            && !cs.instanceId.empty() && cs.instanceId == rs.instanceId
            && !logosctl::processAlive(rs.pid)) {
            output().printError(
                "NO_DAEMON",
                fmt::format("No daemon running (stale state file: pid {} is gone).",
                            rs.pid));
            return 2;
        }
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosMap result = client().shutdown();

    std::string status = result.value("status", std::string{});
    if (status == "error") {
        output().printError(result.value("code", std::string{}),
                            result.value("message", std::string{}), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(fmt::format("Daemon shutdown initiated: {}",
                                      result.value("message", std::string{"ok"})));
    }

    return 0;
}
