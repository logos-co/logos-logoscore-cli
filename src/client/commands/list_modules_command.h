#ifndef LIST_MODULES_COMMAND_H
#define LIST_MODULES_COMMAND_H

#include "command.h"

class ListModulesCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "list-modules"; }
    QString description() const override { return "List available or loaded modules"; }
};

#endif // LIST_MODULES_COMMAND_H
