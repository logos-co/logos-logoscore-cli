#ifndef REMOTE_COMMAND_H
#define REMOTE_COMMAND_H

#include "command.h"

// `logosctl remote ...`: this client's operator pairings with daemons, for
// `logosctl --remote PEER <command>`. Needs no daemon of its own.
class RemoteCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "remote"; }
    std::string description() const override { return "Pair with daemons to operate them remotely"; }
};

#endif // REMOTE_COMMAND_H
