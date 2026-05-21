#ifndef WATCH_COMMAND_H
#define WATCH_COMMAND_H

#include "command.h"

class WatchCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "watch"; }
    std::string description() const override { return "Watch events from a module"; }
};

#endif // WATCH_COMMAND_H
