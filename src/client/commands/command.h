#ifndef COMMAND_H
#define COMMAND_H

#include "../client.h"
#include "../output.h"
#include <QStringList>
#include <memory>
#include <string>
#include <vector>

class Command {
public:
    Command(Client& client, Output& output);
    virtual ~Command() = default;

    virtual int execute(const std::vector<std::string>& args) = 0;
    virtual QString name() const = 0;
    virtual QString description() const = 0;

protected:
    Client& client();
    Output& output();

    // Helper: ensure connected to daemon, print error and return exit code 2 if not
    int ensureConnected();

private:
    Client& m_client;
    Output& m_output;
};

// Factory to create commands by name
std::unique_ptr<Command> createCommand(const QString& name, Client& client, Output& output);

// List of known subcommand names
QStringList knownSubcommands();

#endif // COMMAND_H
