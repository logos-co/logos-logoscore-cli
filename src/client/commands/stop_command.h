#ifndef STOP_COMMAND_H
#define STOP_COMMAND_H

#include "command.h"

class StopCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "stop"; }
    QString description() const override { return "Stop the daemon"; }
};

#endif // STOP_COMMAND_H
