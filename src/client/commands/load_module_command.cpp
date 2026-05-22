#include "load_module_command.h"
#include "../../string_utils.h"
#include <CLI/CLI.hpp>
#include <fmt/format.h>

int LoadModuleCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"load-module"};
    cli.set_help_flag();
    std::string name;
    cli.add_option("name", name, "Module name")->required();
    try {
        auto argsCopy = args;
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore load-module <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosMap result = client().loadModule(name);

    std::string status = result.value("status", std::string{});
    if (status == "error") {
        output().printError(result.value("code", std::string{}),
                            result.value("message", std::string{}), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        std::string version = result.value("version", std::string{});
        LogosList deps      = result.value("dependencies_loaded", LogosList::array());

        output().printRaw(fmt::format("Loaded module: {} (v{})", name, version));
        if (!deps.empty()) {
            std::vector<std::string> depNames;
            for (const auto& v : deps)
                depNames.push_back(v.get<std::string>());
            output().printRaw(fmt::format("  Dependencies loaded: {}",
                                          strutil::join(depNames, ", ")));
        }
    }

    return 0;
}
