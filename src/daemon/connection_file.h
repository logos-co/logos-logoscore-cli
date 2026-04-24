#ifndef CONNECTION_FILE_H
#define CONNECTION_FILE_H

#include <string>
#include <vector>
#include <cstdint>

// Transport endpoint advertised by the daemon for core_service. Clients
// read the `transports` array, pick one (local by default, honoring
// --transport=<proto> when specified), and dial accordingly.
struct TransportInfo {
    std::string protocol;   // "local" | "tcp" | "tcp_ssl"
    std::string host;       // bind address (e.g. "0.0.0.0" or "127.0.0.1")
    uint16_t    port = 0;
    std::string caFile;     // tcp_ssl only
    bool        verifyPeer = true;
    // Wire codec for RPC framing. Only meaningful for plain transports
    // (tcp / tcp_ssl); clients must match what the daemon advertised.
    // Defaults to "json" to keep the existing behavior; additional values
    // like "cbor" become available as new codecs are added to the SDK.
    std::string codec = "json";
};

struct ConnectionInfo {
    // True iff the file on disk exists and parsed as valid JSON with a
    // non-empty instance_id. Says nothing about whether the daemon is
    // actually reachable — that's a separate concern handled by the
    // explicit probes below, which need to know which transport the
    // caller is about to dial.
    bool fileOk = false;
    std::string instanceId;
    std::string token;
    int64_t pid = -1;
    std::string startedAt;           // ISO 8601 UTC timestamp
    std::vector<std::string> modulesDirs;
    std::vector<TransportInfo> transports;
};

class ConnectionFile {
public:
    static bool write(const std::string& instanceId, const std::string& token,
                      int64_t pid, const std::vector<std::string>& modulesDirs,
                      const std::vector<TransportInfo>& transports = {});

    // Pure parse — no liveness check. `fileOk` reflects whether the
    // file is on disk and shape-valid, nothing more. Liveness is
    // answered by actually calling `status` through the RPC — there's
    // no cheaper check that's also correct across every transport
    // (local vs remote PID namespaces, NAT, port-forwarding, …).
    static ConnectionInfo read();
    static bool remove();
    static std::string filePath();
};

#endif // CONNECTION_FILE_H
