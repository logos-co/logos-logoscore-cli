#ifndef COMMAND_H
#define COMMAND_H

#include "../client.h"
#include "../output.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Evidence, gathered without talking to anyone, that the daemon this client
// would dial is already gone: the session's daemon/state.json names a pid that
// is no longer alive.
//
// Worth having as a separate answer because the transport cannot give one. A
// LocalSocket client "connects" to a socket path nobody is listening on -- QtRO
// reports no error for a peer that isn't there -- so an RPC sent into a dead
// session is never answered and never refused, and a missing reply is
// indistinguishable from a slow one until Timeout(20000) fires. Reading the
// session's own bookkeeping is the only way to know sooner.
struct StaleSession {
    int64_t     pid = -1;
    // One clause naming the evidence, e.g. "stale state file: pid 4242 is
    // gone". Callers wrap it in whatever their own output shape calls for.
    std::string reason;
};

// nullopt means "no evidence of a dead daemon", which is NOT the same as "a
// daemon is running" -- it is also what a session with no state.json returns,
// and what a remote client gets. Two deliberate no-ops:
//
//   - No state.json (or no pid in it): nothing to be stale about.
//   - The state file describes a DIFFERENT instance than the client dials.
//     A remote client can have a co-resident daemon's leftovers sitting in its
//     own session directory; those say nothing about the daemon at the far end
//     of its TCP connection, and must not stop it from reaching it. The client
//     config of such a client carries no instance_id at all, so the instance-id
//     match in the definition is what keeps this function silent for it.
std::optional<StaleSession> detectStaleSession();

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

    // Ensure connected to the daemon; print NO_DAEMON and return exit code 2 if
    // not. Every command whose first act is an RPC goes through here, which is
    // what makes this the one place the stale-session guard has to live: it
    // refuses `detectStaleSession()` up front rather than letting the RPC be
    // swallowed by a session nobody is serving. See the comment on the
    // definition for which commands that covers and why none opts out.
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
