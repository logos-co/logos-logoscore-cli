#include "module_info_command.h"
#include <CLI/CLI.hpp>

int ModuleInfoCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"module-info"};
    cli.set_help_flag();
    std::string name;
    cli.add_option("name", name, "Module name")->required();
    try {
        auto argsCopy = args;
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore module-info <name>");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    QJsonObject info = client().getModuleInfo(name);

    std::string status = info.value("status").toString().toStdString();
    if (status == "error") {
        output().printError(info.value("code").toString().toStdString(),
                            info.value("message").toString().toStdString(), info);
        return 3;
    }

    output().printModuleInfo(info);
    return 0;
}
