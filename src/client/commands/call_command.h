#ifndef CALL_COMMAND_H
#define CALL_COMMAND_H

#include "command.h"

#include <optional>
#include <string>

class CallCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "call"; }
    std::string description() const override { return "Call a method on a loaded module"; }

private:
    // Resolves an `@file` reference to the file's contents. For a non-`@` value
    // returns the value unchanged. Returns nullopt only when an `@file` can't be
    // opened — a readable-but-empty file yields an empty string, not nullopt, so
    // callers can tell "couldn't read" apart from "read, but empty".
    std::optional<std::string> resolveFileParam(const std::string& param);
};

#endif // CALL_COMMAND_H
