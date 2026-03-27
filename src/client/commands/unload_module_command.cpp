#include "unload_module_command.h"
#include <CLI/CLI.hpp>

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

    QString moduleName = QString::fromStdString(name);
    QJsonObject result = client().unloadModule(moduleName);

    QString status = result.value("status").toString();
    if (status == "error") {
        output().printError(result.value("code").toString(),
                           result.value("message").toString(), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(QString("Unloaded module: %1").arg(moduleName));
    }

    return 0;
}
