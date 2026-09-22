#include "watch_command.h"
#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>

int WatchCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"watch"};
    cli.set_help_flag();
    std::string module;
    std::string eventName;
    cli.add_option("module", module, "Module name")->required();
    cli.add_option("--event", eventName, "Event name filter (optional)")->default_val("");
    try {
        parseArgs(cli, args);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
                            "Usage: logosctl watch <module> [--event <event>]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    // Shared with the protocol worker, which may still report a loss after a
    // failed watch has returned.
    struct Lost {
        std::mutex mutex;
        std::condition_variable wake;
        bool gone = false;
    };
    auto lost = std::make_shared<Lost>();
    bool ok = client().watchModuleEvents(module, eventName,
        [this](const LogosMap& event) {
            output().printEvent(event);
        },
        [this, module, lost](const std::string& reason) {
            output().printError("NO_DAEMON",
                fmt::format("The daemon went away ({}); stopped watching '{}'.", reason, module));
            {
                std::lock_guard<std::mutex> lock(lost->mutex);
                lost->gone = true;
            }
            lost->wake.notify_all();
        });

    if (!ok) {
        output().printError("WATCH_FAILED",
                            fmt::format("Failed to watch events for module '{}'.", module));
        return 3;
    }

    if (!output().isJsonMode())
        std::cerr << fmt::format("Watching events from '{}'... (Ctrl+C to stop)\n", module);

    // Event delivery is owned by the protocol worker. Keep the command alive
    // until the daemon goes away, or the operating system handles Ctrl+C.
    std::unique_lock<std::mutex> lock(lost->mutex);
    lost->wake.wait(lock, [&] { return lost->gone; });
    return 2;
}
