#ifndef CATALOG_COMMAND_H
#define CATALOG_COMMAND_H

#include "command.h"

// The `catalog` group: ls, add, remove, enable, disable, refresh.
//
// "Catalog" is the CLI's word for what the downloader library and module call
// a repository — the layers below keep their own vocabulary, since renaming
// them would be churn with no user-visible benefit.
//
// enable/disable earn their place next to add/remove because the built-in
// default catalog cannot be removed, only silenced.
class CatalogCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "catalog"; }
    std::string description() const override {
        return "Manage the package catalogs this session pulls from";
    }
};

// The `key` group: ls, add, remove — the trusted signing keys used to verify
// package signatures. Per-session, so two sessions can hold different trust.
class KeyCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "key"; }
    std::string description() const override {
        return "Manage trusted package-signing keys";
    }
};

#endif // CATALOG_COMMAND_H
