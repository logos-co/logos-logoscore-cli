#include "list_modules_command.h"
#include <CLI/CLI.hpp>

int ListModulesCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"list-modules"};
    cli.set_help_flag();
    bool loadedOnly = false;
    cli.add_flag("--loaded", loadedOnly, "Show only loaded modules");
    try {
        parseArgs(cli, args);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logosctl module ls [--loaded]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    std::string filter = loadedOnly ? "loaded" : "all";
    LogosList modules = client().listModules(filter);
    output().printModuleList(modules);
    return 0;
}
