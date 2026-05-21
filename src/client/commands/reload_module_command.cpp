#include "reload_module_command.h"
#include <CLI/CLI.hpp>
#include <cstdio>

int ReloadModuleCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"reload-module"};
    cli.set_help_flag();
    std::string name;
    cli.add_option("name", name, "Module name")->required();
    try {
        auto argsCopy = args;
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore reload-module <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    if (!output().isJsonMode())
        fprintf(stderr, "Reloading %s...\n", name.c_str());

    QJsonObject result = client().reloadModule(name);

    std::string status = result.value("status").toString().toStdString();
    if (status == "error") {
        output().printError(result.value("code").toString().toStdString(),
                            result.value("message").toString().toStdString(), result);
        return 3;
    }

    output().printReload(result);
    return 0;
}
