#ifndef LIST_MODULES_COMMAND_H
#define LIST_MODULES_COMMAND_H

#include "command.h"

class ListModulesCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "list-modules"; }
    std::string description() const override { return "List available or loaded modules"; }
};

#endif // LIST_MODULES_COMMAND_H
