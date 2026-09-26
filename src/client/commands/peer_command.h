#ifndef PEER_COMMAND_H
#define PEER_COMMAND_H

#include "command.h"

#include <optional>
#include <string>

// `logosctl peer ...`: links with other Logos runtimes, through peering_module.
// Every verb is one peering_module method, forwarded by core_service as this
// operator; peering_module decides what an operator may change.
class PeerCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "peer"; }
    std::string description() const override { return "Link with other Logos runtimes"; }

private:
    // peering_module's answer, or nullopt after printing the failure.
    std::optional<LogosMap> call(const std::string& method, const LogosList& args);
    // A runtime id for an alias or id, from the enrolled peers.
    std::optional<std::string> resolvePeer(const std::string& peer);
    int print(const LogosMap& result);
};

#endif // PEER_COMMAND_H
