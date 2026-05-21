#ifndef STATUS_COMMAND_H
#define STATUS_COMMAND_H

#include "command.h"

class StatusCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "status"; }
    std::string description() const override { return "Show daemon and module health"; }
};

#endif // STATUS_COMMAND_H
