#include "command.h"
#include "status_command.h"
#include "load_module_command.h"
#include "unload_module_command.h"
#include "reload_module_command.h"
#include "list_modules_command.h"
#include "module_info_command.h"
#include "call_command.h"
#include "watch_command.h"
#include "stats_command.h"
#include "stop_command.h"
#include "issue_token_command.h"
#include "revoke_token_command.h"
#include "list_tokens_command.h"
#include "package_command.h"
#include "catalog_command.h"
#include "config_command.h"

#include "../client_state.h"
#include "../../daemon/daemon_state.h"
#include "../../process_util.h"

#include <fmt/format.h>

std::optional<StaleSession> detectStaleSession()
{
    const DaemonRuntimeState rs = DaemonRuntimeStateFile::read();

    // Nothing has ever announced itself in this session, or it announced
    // itself without a usable pid. Either way there is no claim to disprove.
    if (!rs.fileOk || rs.pid <= 0)
        return std::nullopt;

    // The liveness question first, because it is a syscall and the one below
    // is a file read and a YAML parse. It also keeps the common path -- a
    // daemon that is running -- from reading client/config.yaml twice per
    // command, once here and once in RpcClient::connect, which for a
    // malformed config would mean printing the same parse error twice.
    if (logosctl::processAlive(rs.pid))
        return std::nullopt;

    // Only when the state file describes the daemon this client would dial.
    // See the header: a remote client's session can hold a co-resident
    // daemon's leftovers, and its own config carries no instance_id, so an
    // empty one must never match.
    const ClientState cs = ClientStateFile::read();
    if (cs.instanceId.empty() || cs.instanceId != rs.instanceId)
        return std::nullopt;

    return StaleSession{
        rs.pid,
        fmt::format("stale state file: pid {} is gone", rs.pid),
    };
}

Command::Command(Client& client, Output& output)
    : m_client(client), m_output(output)
{
}

Client& Command::client()
{
    return m_client;
}

Output& Command::output()
{
    return m_output;
}

int Command::ensureConnected()
{
    if (m_client.isConnected())
        return 0;

    // Refuse a session whose daemon is provably gone BEFORE dialling it.
    //
    // Connecting is not the check people assume it is: a LocalSocket client
    // succeeds against a socket path with no listener, so every command past
    // this point used to sit out the full RPC deadline -- Timeout(20000) in
    // logos-protocol's cpp/logos_mode.h -- and only then report a failure the
    // session directory could have named instantly.
    //
    // This covers every command that opens with an RPC, because they all reach
    // the wire through here: call, catalog, key, list-modules, load-module,
    // module-info, package (all six of its RPC paths), reload-module, stats,
    // stop, unload-module, watch. StatusCommand runs the same check itself,
    // one step earlier, because its answer to "no daemon" is a status report
    // rather than an error. The token commands (issue/revoke/list) and the
    // config commands never call this: they read and write the session's own
    // files and have no daemon to be absent.
    //
    // Nothing opts out. `watch` is the one command with a case for waiting --
    // a daemon that has not started yet is a reasonable thing to watch for --
    // but it does no waiting today: it connects once and gives up, so failing
    // in milliseconds instead of twenty seconds is strictly what it already
    // meant to do. Give it a retry loop and it should skip this guard
    // explicitly and poll `detectStaleSession()` instead.
    if (const std::optional<StaleSession> stale = detectStaleSession()) {
        m_output.printError(
            "NO_DAEMON",
            fmt::format("No daemon running ({}).", stale->reason));
        return 2;
    }

    if (!m_client.connect()) {
        m_output.printError("NO_DAEMON", m_client.lastError());
        return 2;
    }
    return 0;
}

std::vector<std::string> knownSubcommands()
{
    return {
        "daemon", "status",
        "load-module", "unload-module", "reload-module",
        "list-modules", "module-info", "info",
        "call", "module",  // "module" for verbose call syntax
        "watch", "stats", "stop",
        "issue-token", "revoke-token", "list-tokens",
        "package", "catalog", "key",
        "daemon-config", "client-config", "client"
    };
}

std::unique_ptr<Command> createCommand(const std::string& name, Client& client, Output& output)
{
    if (name == "status")
        return std::make_unique<StatusCommand>(client, output);
    if (name == "load-module")
        return std::make_unique<LoadModuleCommand>(client, output);
    if (name == "unload-module")
        return std::make_unique<UnloadModuleCommand>(client, output);
    if (name == "reload-module")
        return std::make_unique<ReloadModuleCommand>(client, output);
    if (name == "list-modules")
        return std::make_unique<ListModulesCommand>(client, output);
    if (name == "module-info" || name == "info")
        return std::make_unique<ModuleInfoCommand>(client, output);
    if (name == "call" || name == "module")
        return std::make_unique<CallCommand>(client, output);
    if (name == "watch")
        return std::make_unique<WatchCommand>(client, output);
    if (name == "stats")
        return std::make_unique<StatsCommand>(client, output);
    if (name == "stop")
        return std::make_unique<StopCommand>(client, output);
    if (name == "issue-token")
        return std::make_unique<IssueTokenCommand>(client, output);
    if (name == "revoke-token")
        return std::make_unique<RevokeTokenCommand>(client, output);
    if (name == "list-tokens")
        return std::make_unique<ListTokensCommand>(client, output);
    if (name == "package")
        return std::make_unique<PackageCommand>(client, output);
    if (name == "catalog")
        return std::make_unique<CatalogCommand>(client, output);
    if (name == "key")
        return std::make_unique<KeyCommand>(client, output);
    if (name == "daemon-config")
        return std::make_unique<ConfigCommand>(client, output, /*daemonSide=*/true);
    if (name == "client-config" || name == "client")
        return std::make_unique<ConfigCommand>(client, output, /*daemonSide=*/false);

    return nullptr;
}
