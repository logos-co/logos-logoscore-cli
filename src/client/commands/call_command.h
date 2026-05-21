#ifndef CALL_COMMAND_H
#define CALL_COMMAND_H

#include "command.h"

class CallCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "call"; }
    std::string description() const override { return "Call a method on a loaded module"; }

private:
    std::string resolveFileParam(const std::string& param);
};

#endif // CALL_COMMAND_H
