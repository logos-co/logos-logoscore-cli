#ifndef STATS_COMMAND_H
#define STATS_COMMAND_H

#include "command.h"

class StatsCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "stats"; }
    std::string description() const override { return "Show module resource usage"; }
};

#endif // STATS_COMMAND_H
