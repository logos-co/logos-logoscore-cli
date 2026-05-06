#ifndef LIST_TOKENS_COMMAND_H
#define LIST_TOKENS_COMMAND_H

#include "command.h"

// `logoscore list-tokens` — names + issued_at timestamps from
// daemon/tokens.json. Raw tokens are never shown (they're only
// persisted in daemon/tokens/<name>.json on issuance; afterwards
// they only exist hashed in tokens.json).
class ListTokensCommand : public Command {
public:
    using Command::Command;
    int execute(const std::vector<std::string>& args) override;
    QString name() const override { return "list-tokens"; }
    QString description() const override {
        return "List the names of issued client tokens";
    }
};

#endif // LIST_TOKENS_COMMAND_H
