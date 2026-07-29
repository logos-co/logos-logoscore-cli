#ifndef CONFIG_COMMAND_H
#define CONFIG_COMMAND_H

#include "command.h"

// `daemon config set|show` and `client config set|show`.
//
// Configuration is installed by its own command and never rides along on an
// unrelated one, so `daemon start` and every client command take the session
// exactly as it is on disk. `set` overwrites wholesale — there is no merge to
// reason about, and no way to end up with a half-applied config.
//
// Both operate purely on the session directory, so they work with no daemon
// running (which is the normal case: you configure a session, then start it).
class ConfigCommand : public Command {
public:
    // `daemonSide` picks which of the two files this instance owns. They are
    // kept as separate documents to preserve the existing ownership rule: the
    // daemon never reads client/, and the client never reads daemon/.
    ConfigCommand(Client& client, Output& output, bool daemonSide)
        : Command(client, output), m_daemonSide(daemonSide) {}

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return m_daemonSide ? "daemon-config" : "client-config"; }
    std::string description() const override {
        return m_daemonSide ? "Show or replace the daemon configuration"
                            : "Show or replace the client dial spec";
    }

private:
    int set(const std::vector<std::string>& args);
    int show();

    bool m_daemonSide;
};

#endif // CONFIG_COMMAND_H
