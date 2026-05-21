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

    QJsonObject result = client().loadModule(name);

    std::string status = result.value("status").toString().toStdString();
    if (status == "error") {
        output().printError(result.value("code").toString().toStdString(),
                            result.value("message").toString().toStdString(), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        std::string version = result.value("version").toString().toStdString();
        QJsonArray deps     = result.value("dependencies_loaded").toArray();

        output().printRaw(fmt::format("Loaded module: {} (v{})", name, version));
        if (!deps.isEmpty()) {
            std::vector<std::string> depNames;
            for (const QJsonValue& v : deps)
                depNames.push_back(v.toString().toStdString());
            output().printRaw(fmt::format("  Dependencies loaded: {}",
                                          strutil::join(depNames, ", ")));
        }
    }

    return 0;
}
