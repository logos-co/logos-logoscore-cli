#include "watch_command.h"
#include <CLI/CLI.hpp>
#include <QCoreApplication>
#include <fmt/format.h>
#include <csignal>

static volatile sig_atomic_t g_watchInterrupted = 0;

static void watchSignalHandler(int)
{
    g_watchInterrupted = 1;
}

int WatchCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"watch"};
    cli.set_help_flag();
    std::string module;
    std::string event;
    cli.add_option("module", module, "Module name")->required();
    cli.add_option("--event", event, "Event name to filter");
    try {
        std::vector<std::string> argsCopy(args.rbegin(), args.rend());
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
                            "Usage: logoscore watch <module> [--event <name>]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    g_watchInterrupted = 0;
    struct sigaction sa;
    sa.sa_handler = watchSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    bool watching = client().watchModuleEvents(module, event,
        [this](const QJsonObject& ev) {
            output().printEvent(ev);
        });

    if (!watching) {
        output().printError("MODULE_NOT_LOADED",
                            fmt::format("Module '{}' is not loaded. "
                                        "Load it with: logoscore load-module {}",
                                        module, module));
        return 3;
    }

    while (!g_watchInterrupted)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);

    return 0;
}
