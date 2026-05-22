#include "watch_command.h"
#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <QCoreApplication>
#include <iostream>

int WatchCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"watch"};
    cli.set_help_flag();
    std::string module;
    std::string eventName;
    cli.add_option("module", module, "Module name")->required();
    cli.add_option("--event", eventName, "Event name filter (optional)")->default_val("");
    try {
        std::vector<std::string> argsCopy(args.rbegin(), args.rend());
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
                            "Usage: logoscore watch <module> [--event <event>]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    bool ok = client().watchModuleEvents(module, eventName,
        [this](const LogosMap& event) {
            output().printEvent(event);
        });

    if (!ok) {
        output().printError("WATCH_FAILED",
                            fmt::format("Failed to watch events for module '{}'.", module));
        return 3;
    }

    if (!output().isJsonMode())
        std::cerr << fmt::format("Watching events from '{}'... (Ctrl+C to stop)\n", module);

    QCoreApplication::exec();
    return 0;
}
