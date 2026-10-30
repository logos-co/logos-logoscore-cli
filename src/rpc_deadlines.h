#pragma once

#include <logos_mode.h>

#include <limits>
#include <string>

// Deadlines for the calls that leave the machine, walk a whole package, or stop
// modules. The transport default (Timeout(), 20 s) fits a status query, not a
// 150 MB module.
// Both legs draw from here: the client's wait on the daemon, and the daemon's
// own wait on the package modules, which used to run on the default. The
// client's leg is the longer one by kReplyMarginMs -- see clientDeadlineFor.
namespace rpc_deadlines {

constexpr int kCatalogMs  = 2  * 60 * 1000;   // fetch and resolve against the catalog
constexpr int kTransferMs = 30 * 60 * 1000;   // download, inspect, or install a package
// Unload stops modules one at a time, dependents first, and each may take up to
// 14 s: the container's 12 s stop window plus its 2 s SIGKILL wait.
constexpr int kUnloadMs   = 2  * 60 * 1000;

// How much longer the client waits than the daemon-side call it is waiting on.
//
// The two legs used to expire together, and the client's always started first:
// it is already waiting while the request crosses to the daemon, and the SDK
// starts the daemon's own call clock only after it has acquired the target and
// minted a token for it (logos_api_client.cpp -- the deadline is per phase, not
// end to end). So a call that ran out its budget reported a coin flip: either
// the daemon's METHOD_FAILED naming the timeout, or the client's bare
// RPC_FAILED, which cannot even say whether the method ran.
//
// The margin covers that dispatch and handshake plus the reply's trip back,
// with room for a remote transport. It costs nothing when the daemon answers:
// the client returns the moment the reply lands, so only a daemon that has
// stopped answering altogether makes anyone wait it out. 5 s for the same
// reason as the shutdown reply window: an answer later than that is not coming.
constexpr int kReplyMarginMs = 5 * 1000;

// The client's deadline for a call that wraps a daemon-side call of `daemonMs`.
// Saturates rather than overflowing: `--timeout` accepts anything up to INT_MAX.
// (max) in parentheses: <windows.h> defines a max() macro, and process_util.h
// pulls it in ahead of this header on Windows.
constexpr int clientDeadlineFor(int daemonMs)
{
    return daemonMs > (std::numeric_limits<int>::max)() - kReplyMarginMs
               ? (std::numeric_limits<int>::max)()
               : daemonMs + kReplyMarginMs;
}

// The daemon's deadline for one call into package_manager / package_downloader.
// The heavy methods do the entire transfer or archive walk inside the call.
inline Timeout forPackageCall(const std::string& method)
{
    if (method == "downloadResolvedDependencies" || method == "downloadPinned"
        || method == "inspectPackage" || method == "installPlugin")
        return Timeout(kTransferMs);
    if (method == "resolveDependencies")
        return Timeout(kCatalogMs);
    return Timeout();
}

} // namespace rpc_deadlines
