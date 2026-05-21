#ifndef REVOKE_TOKEN_COMMAND_H
#define REVOKE_TOKEN_COMMAND_H

#include "command.h"

// `logoscore revoke-token NAME` — removes the token from
// daemon/tokens.json and deletes daemon/tokens/<name>.json. Exit 3
// if no token with that name exists.
class RevokeTokenCommand : public Command {
public:
    using Command::Command;
    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "revoke-token"; }
    std::string description() const override {
        return "Revoke a previously-issued client token";
    }
};

#endif // REVOKE_TOKEN_COMMAND_H
