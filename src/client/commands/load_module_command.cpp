#include "load_module_command.h"
#include <CLI/CLI.hpp>

int LoadModuleCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"load-module"};
    cli.set_help_flag();  // help handled at top level
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

    QString moduleName = QString::fromStdString(name);
    QJsonObject result = client().loadModule(moduleName);

    QString status = result.value("status").toString();
    if (status == "error") {
        QString code = result.value("code").toString();
        output().printError(code, result.value("message").toString(), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        QString version = result.value("version").toString();
        QJsonArray deps = result.value("dependencies_loaded").toArray();

        output().printRaw(QString("Loaded module: %1 (v%2)").arg(moduleName, version));
        if (!deps.isEmpty()) {
            QStringList depNames;
            for (const QJsonValue& v : deps)
                depNames.append(v.toString());
            output().printRaw(QString("  Dependencies loaded: %1").arg(depNames.join(", ")));
        }
    }

    return 0;
}
