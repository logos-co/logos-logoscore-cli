#include "unload_module_command.h"
#include <CLI/CLI.hpp>
#include <fmt/format.h>

int UnloadModuleCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"unload-module"};
    cli.set_help_flag();
    std::string name;
    cli.add_option("name", name, "Module name")->required();
    try {
        auto argsCopy = args;
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore unload-module <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosMap result = client().unloadModule(name);

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
    }

    return 0;
}
