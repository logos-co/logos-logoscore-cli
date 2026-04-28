#ifndef CLIENT_CONNECTION_H
#define CLIENT_CONNECTION_H

#include "../daemon/daemon_state.h"

#include <optional>
#include <string>
#include <vector>

// Client-side transport resolution.
//
// `DaemonStateFile` owns the on-disk shape: parse, serialize, nothing more.
// Deciding *what endpoint the client should actually dial* is a separate
// concern — the daemon may have advertised one address (the one it bound)
// while the client needs to dial a different one (docker port-forwarding,
// NAT, SSH tunnels). That resolution is a client-side decision driven by
// env vars set from CLI flags (`--client-tcp-host`, `--client-tcp-port`,
// `--no-verify-peer`) or by the Python wrapper.
//
// Both `src/client/client.cpp` (for normal RPC) and the status command
// path need the same answer, which is why this lives in its own unit
// instead of being inlined at one call site.
namespace ClientConnection {

// Apply client-side environment overrides to a daemon-advertised transport.
// Only tcp / tcp_ssl entries are affected — `local` is returned unchanged.
//
//   LOGOSCORE_CLIENT_TCP_HOST       → overrides transport.host
//   LOGOSCORE_CLIENT_TCP_PORT       → overrides transport.port (ignored if
//                                     unparseable or out of [1, 65535])
//   LOGOSCORE_CLIENT_NO_VERIFY_PEER → flips verifyPeer off (tcp_ssl only in
//                                     practice, but the flag isn't gated)
TransportInfo effectiveTransport(const TransportInfo& advertised);

// Resolve which protocol the client should pick for `moduleName` from the
// daemon's advertised list, honoring per-module overrides:
//
//   1. LOGOSCORE_CLIENT_TRANSPORT_<MODULE_UPPER>  (per-module env var)
//   2. LOGOSCORE_CLIENT_TRANSPORT                 (process-wide default)
//   3. "local"                                    (final fallback)
//
// Module-name lookup is case-insensitive on the env-var side: a module
// named `core_service` is matched by `LOGOSCORE_CLIENT_TRANSPORT_CORE_SERVICE`
// (uppercased, hyphens→underscores). Returns the picked protocol string
// ("local" | "tcp" | "tcp_ssl") or std::nullopt if the daemon advertised
// nothing matching the resolved preference (caller can decide whether
// to fall back further or report an error).
std::optional<std::string> pickTransportForModule(
    const std::string& moduleName,
    const std::vector<TransportInfo>& advertised);

} // namespace ClientConnection

#endif // CLIENT_CONNECTION_H
