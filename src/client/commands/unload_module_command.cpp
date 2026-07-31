#include "unload_module_command.h"
#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

int UnloadModuleCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"unload-module"};
    cli.set_help_flag();
    std::string name;
    cli.add_option("name", name, "Module name")->required();
    // The cascade is the default: leaving dependents running against an
    // unloaded provider is the more surprising outcome, so opting out is
    // explicit.
    bool noDependents = false;
    cli.add_flag("--no-dependents", noDependents,
                 "Unload only this module, leaving its dependents running");
    try {
        parseArgs(cli, args);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
                            "Usage: logosctl module unload <name> [--no-dependents]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosMap result = client().unloadModule(name, !noDependents);

    std::string status = result.value("status", std::string{});
    if (status == "error") {
        output().printError(result.value("code", std::string{}),
                            result.value("message", std::string{}), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(fmt::format("Unloaded module: {}", name));
        // Surface the cascade — silently taking down three other modules is
        // exactly the kind of thing a human needs told.
        auto dependents = result.value("dependents_unloaded", LogosList::array());
        if (dependents.is_array() && !dependents.empty()) {
            std::vector<std::string> names;
            for (const auto& d : dependents)
                names.push_back(d.get<std::string>());
            output().printRaw(fmt::format("Also unloaded {} dependent(s): {}",
                                          names.size(), fmt::join(names, ", ")));
        }
    }

    return 0;
}
