#ifndef LOGOSCORE_CLIENT_STATE_H
#define LOGOSCORE_CLIENT_STATE_H

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <set>
#include <string>

// client/config.json schema version. Aligned with the daemon-side
// v2 (config.json / state.json / tokens.json) for symmetry across
// the four config-tree files. Bump when the on-disk shape changes;
// readers reject anything else with a clear regenerate message.
constexpr int kClientStateSchemaVersion = 2;

// In-memory representation of <configDir>/client/config.json. Owned
// and rewritten by client subcommands; the daemon never reads or
// writes this struct.
struct ClientState {
    bool fileOk = false;
    int  schemaVersion = 0;

    // Filename inside <configDir>/client/ pointing at the raw-token
    // file this client uses. The file must exist or the client
    // refuses to start.
    std::string tokenFile;

    // Daemon instance id: the local socket is `logos_<module>_<instance_id>`.
    // The daemon writes it here each boot; empty leaves it to $LOGOS_INSTANCE_ID.
    std::string instanceId;

    // The modules this client dials, each on its local socket
    // (`daemon.<module>.transport: local`). `core_service` is required.
    std::set<std::string> daemon;
};

// Validate a client-config document (YAML already converted to JSON) and turn
// it into a ClientState, without touching disk. Returns nullopt with a one-line
// reason in `error` for anything the client could not dial from: a wrong
// top-level type, an unsupported schema version, a value of the wrong type, or
// a `daemon.<module>` entry that is not `transport: local` (tcp and tcp_ssl
// were removed). The message names the offending key by its dotted path.
//
// Never throws: a hand-written config must not be able to terminate the
// process. Shared by ClientStateFile::read and by `client config set`, which
// validates a document through this *before* writing it.
std::optional<ClientState> parseClientStateDocument(const nlohmann::json& obj,
                                                    std::string* error);

class ClientStateFile {
public:
    static std::string filePath();

    // Read the on-disk client/config.json (or return an in-process
    // override if one was set via setOverride). Both consumers
    // (RpcClient::connect, status command's "not_configured" probe)
    // go through this single entry point so an override applies
    // uniformly to whichever fires first.
    static ClientState read();
    static bool write(const ClientState& state);

    // Inject a pre-merged ClientState that subsequent read() calls
    // will return verbatim, bypassing disk. Used by main.cpp when
    // CLI client-config flags are passed but `--persist-config`
    // wasn't: the flags affect this run only, no disk write. Pass
    // std::nullopt to clear (only needed in tests; the override
    // is process-wide and isn't reset between subcommand
    // dispatches in normal flow).
    static void setOverride(std::optional<ClientState> override);

    // Read the raw token from <configDir>/client/<tokenFile>. Returns
    // an empty string if the file is missing or malformed. The token
    // is the `token` field of the JSON object, matching the shape
    // emitted by the daemon's auto-issue path and by `cp` of a
    // daemon/tokens/<name>.json file.
    static std::string readTokenFile(const std::string& filename);
};

#endif // LOGOSCORE_CLIENT_STATE_H
