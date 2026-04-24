#ifndef CLIENT_CONNECTION_H
#define CLIENT_CONNECTION_H

#include "../daemon/connection_file.h"

// Client-side transport resolution.
//
// `ConnectionFile` owns the on-disk shape: parse, serialize, nothing more.
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

} // namespace ClientConnection

#endif // CLIENT_CONNECTION_H
