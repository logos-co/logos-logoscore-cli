#ifndef MODULE_INFO_COMMAND_H
#define MODULE_INFO_COMMAND_H

#include "command.h"

class ModuleInfoCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    QString name() const override { return "module-info"; }
    QString description() const override { return "Show detailed module information"; }
};

#endif // MODULE_INFO_COMMAND_H
