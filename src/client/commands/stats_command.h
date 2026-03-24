#ifndef STATS_COMMAND_H
#define STATS_COMMAND_H

#include "command.h"

class StatsCommand : public Command {
public:
    using Command::Command;

    int execute(const QStringList& args) override;
    QString name() const override { return "stats"; }
    QString description() const override { return "Show module resource usage"; }
};

#endif // STATS_COMMAND_H
