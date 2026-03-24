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

    if (!m_client.connect()) {
        m_output.printError("NO_DAEMON", m_client.lastError());
        return 2;
    }
    return 0;
}

QStringList knownSubcommands()
{
    return {
        "daemon", "status",
        "load-module", "unload-module", "reload-module",
        "list-modules", "module-info", "info",
        "call", "module",  // "module" for verbose call syntax
        "watch", "stats", "stop"
    };
}

std::unique_ptr<Command> createCommand(const QString& name, Client& client, Output& output)
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

    return nullptr;
}
