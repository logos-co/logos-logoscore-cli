#ifndef UNLOAD_MODULE_COMMAND_H
#define UNLOAD_MODULE_COMMAND_H

#include "command.h"

class UnloadModuleCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    QString name() const override { return "unload-module"; }
    QString description() const override { return "Unload a module from the daemon"; }
};

#endif // UNLOAD_MODULE_COMMAND_H
