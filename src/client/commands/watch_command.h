#ifndef WATCH_COMMAND_H
#define WATCH_COMMAND_H

#include "command.h"

class WatchCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "watch"; }
    QString description() const override { return "Watch events from a module"; }
};

#endif // WATCH_COMMAND_H
