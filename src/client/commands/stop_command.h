#ifndef STOP_COMMAND_H
#define STOP_COMMAND_H

#include "command.h"

class StopCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "stop"; }
    std::string description() const override { return "Stop the daemon"; }
};

#endif // STOP_COMMAND_H
