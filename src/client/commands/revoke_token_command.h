#ifndef REVOKE_TOKEN_COMMAND_H
#define REVOKE_TOKEN_COMMAND_H

#include "command.h"

// `logoscore revoke-token NAME` — removes the token from tokens.db and
// deletes tokens/<name>.json. Exit 3 if no token with that name exists.
class RevokeTokenCommand : public Command {
public:
    using Command::Command;
    int execute(const std::vector<std::string>& args) override;
    QString name() const override { return "revoke-token"; }
    QString description() const override {
        return "Revoke a previously-issued client token";
    }
};

#endif // REVOKE_TOKEN_COMMAND_H
