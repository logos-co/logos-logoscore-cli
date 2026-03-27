#include "list_modules_command.h"
#include <CLI/CLI.hpp>

int ListModulesCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"list-modules"};
    cli.set_help_flag();
    bool loaded = false;
    cli.add_flag("--loaded", loaded, "Show only loaded modules");
    try {
        auto argsCopy = args;
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore list-modules [--loaded]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    QString filter = loaded ? "loaded" : "all";
    QJsonArray modules = client().listModules(filter);

    output().printModuleList(modules);
    return 0;
}
