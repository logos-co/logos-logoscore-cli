#ifndef COMMAND_H
#define COMMAND_H

#include "../client.h"
#include "../output.h"
#include <memory>
#include <string>
#include <vector>

class Command {
public:
    Command(Client& client, Output& output);
    virtual ~Command() = default;

    virtual int execute(const std::vector<std::string>& args) = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;

protected:
    Client& client();
    Output& output();

    // Helper: ensure connected to daemon, print error and return exit code 2 if not
    int ensureConnected();

private:
    Client& m_client;
    Output& m_output;
};

// Hand `args` to a CLI::App in the order it actually expects.
//
// CLI11's `parse(std::vector<std::string>&)` consumes the vector from the
// BACK, so it wants the arguments reversed; only the rvalue overload reverses
// for you. Passing a natural-order lvalue silently parses the command line
// backwards -- harmless when there is one positional and the rest are flags,
// and quietly wrong the moment an option takes a value, because the option
// then pairs with the token to its left. `package download pkg -o dir` came
// out as pkg="dir", output="pkg".
//
// Defined as a template so this header does not have to include CLI11, which
// is heavy and only needed by the command .cpp files.
template <typename App>
void parseArgs(App& cli, const std::vector<std::string>& args)
{
    std::vector<std::string> reversed(args.rbegin(), args.rend());
    cli.parse(reversed);
}

// Factory to create commands by name
std::unique_ptr<Command> createCommand(const std::string& name, Client& client, Output& output);

// List of known subcommand names
std::vector<std::string> knownSubcommands();

#endif // COMMAND_H
