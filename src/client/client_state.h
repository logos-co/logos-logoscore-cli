#ifndef LOGOSCORE_CLIENT_STATE_H
#define LOGOSCORE_CLIENT_STATE_H

#include <map>
#include <optional>
#include <string>
#include <cstdint>

// client/client.json schema version. Independent of the daemon-state
// schema — bump this if you change the client.json shape, leaving
// daemon.json's version untouched. v1 is the post-config-split schema.
constexpr int kClientStateSchemaVersion = 1;

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

// In-memory representation of <configDir>/client/client.json. Owned
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
    // socket. Daemon's auto-emitted client.json populates this so
    // the local-client path doesn't need to crack open daemon.json.
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
    static ClientState read();
    static bool write(const ClientState& state);

    // Read the raw token from <configDir>/client/<tokenFile>. Returns
    // an empty string if the file is missing or malformed. The token
    // is the `token` field of the JSON object, matching the shape
    // emitted by the daemon's auto-issue path and by `cp` of a
    // daemon/tokens/<name>.json file.
    static std::string readTokenFile(const std::string& filename);
};

#endif // LOGOSCORE_CLIENT_STATE_H
