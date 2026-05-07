#ifndef LOGOSCORE_CLIENT_STATE_H
#define LOGOSCORE_CLIENT_STATE_H

#include <map>
#include <optional>
#include <string>
#include <cstdint>

// client/config.json schema version. Aligned with the daemon-side
// v2 (config.json / state.json / tokens.json) for symmetry across
// the four config-tree files. Bump when the on-disk shape changes;
// readers reject anything else with a clear regenerate message.
constexpr int kClientStateSchemaVersion = 2;

// One resolved transport entry per module the client dials. Mirrors
// the daemon's TransportInfo but is "client-side" — host/port/etc.
// reflect the *dial* address rather than the bind address (which can
// differ behind NAT, docker port-forwarding, SSH tunnels).
struct ClientModuleTransport {
    std::string protocol;   // "local" | "tcp" | "tcp_ssl"
    std::string host;       // tcp / tcp_ssl
    uint16_t    port = 0;   // tcp / tcp_ssl
    std::string codec = "json";
    std::string caFile;     // tcp_ssl
    bool        verifyPeer = true; // tcp_ssl
};

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

    // Daemon instance id. Required for the LocalSocket dial path —
    // the registry name is `local:logos_<module>_<instance_id>`. May
    // be empty for remote clients dialing over TCP / TCP-SSL since
    // the registry name there is the TCP endpoint, not the local
    // socket. Daemon's auto-emitted client/config.json populates
    // this so the local-client path doesn't need to read daemon-side
    // files (daemon/state.json carries the same instance_id, but the
    // client never reads it during normal RPC).
    std::string instanceId;

    // Per-module dial spec. `core_service` and `capability_module`
    // are the two entries the SDK currently consults; future
    // auto-dialed modules add keys here without changing the
    // surrounding shape.
    std::map<std::string, ClientModuleTransport> daemon;
};

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
