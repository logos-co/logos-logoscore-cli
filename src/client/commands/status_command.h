#ifndef STATUS_COMMAND_H
#define STATUS_COMMAND_H

#include "command.h"

class StatusCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "status"; }
    QString description() const override { return "Show daemon and module health"; }
};

#endif // STATUS_COMMAND_H
