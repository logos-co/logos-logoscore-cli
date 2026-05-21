#ifndef ISSUE_TOKEN_COMMAND_H
#define ISSUE_TOKEN_COMMAND_H

#include "command.h"

// `logoscore issue-token --name NAME [--replace]`
//
// Operates directly on $CONFIG_DIR — doesn't require a running daemon.
// Adds {name → hashed_token} to daemon/tokens.json and writes
// daemon/tokens/<name>.json with the raw token (the only place it
// ever appears on disk).
class IssueTokenCommand : public Command {
public:
    using Command::Command;
    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "issue-token"; }
    std::string description() const override {
        return "Issue a new client token under a given name";
    }
};

#endif // ISSUE_TOKEN_COMMAND_H
