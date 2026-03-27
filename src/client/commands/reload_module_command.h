#ifndef RELOAD_MODULE_COMMAND_H
#define RELOAD_MODULE_COMMAND_H

#include "command.h"

class ReloadModuleCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    QString name() const override { return "reload-module"; }
    QString description() const override { return "Reload (unload + load) a module"; }
};

#endif // RELOAD_MODULE_COMMAND_H
