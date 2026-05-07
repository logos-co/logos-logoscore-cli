#ifndef LOGOSCORE_PORT_ALLOCATOR_H
#define LOGOSCORE_PORT_ALLOCATOR_H

#include <cstdint>
#include <string>

// Pre-allocate an ephemeral TCP port on `host` so two listeners that
// would both auto-assign (port=0) don't collide. Used when the daemon
// inherits one transport set across multiple modules: each module
// needs a distinct port and the parent has to know what port the
// child will bind, so we reserve via bind(0)+getsockname()+close()
// before spawning the child.
//
// Returns the chosen port (in host byte order) on success, or 0 on
// failure (host unresolvable, no free ports, etc.). The race window
// between close-here and bind-in-child is the standard pre-allocation
// race — small enough to be the universally-used pattern (status-go,
// the python wrapper's _pick_free_port, etc.), but the caller should
// handle a child bind() failure as a retryable error.
//
// `host` is a numeric address ("127.0.0.1", "0.0.0.0") or an empty
// string (treated as "0.0.0.0"). Hostname resolution is not attempted —
// callers should pre-resolve.
namespace PortAllocator {

uint16_t allocateEphemeralTcp(const std::string& host);

}

#endif // LOGOSCORE_PORT_ALLOCATOR_H
