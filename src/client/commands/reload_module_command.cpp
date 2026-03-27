#include "reload_module_command.h"
#include <CLI/CLI.hpp>

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

    QString moduleName = QString::fromStdString(name);

    if (!output().isJsonMode()) {
        // Print progress to stderr in human mode
        fprintf(stderr, "Reloading %s...\n", moduleName.toUtf8().constData());
    }

    QJsonObject result = client().reloadModule(moduleName);

    QString status = result.value("status").toString();
    if (status == "error") {
        output().printError(result.value("code").toString(),
                           result.value("message").toString(), result);
        return 3;
    }

    output().printReload(result);
    return 0;
}
