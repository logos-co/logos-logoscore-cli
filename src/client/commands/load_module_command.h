#ifndef LOAD_MODULE_COMMAND_H
#define LOAD_MODULE_COMMAND_H

#include "command.h"

class LoadModuleCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "load-module"; }
    QString description() const override { return "Load a module into the daemon"; }
};

#endif // LOAD_MODULE_COMMAND_H
