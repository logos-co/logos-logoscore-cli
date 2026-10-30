#pragma once

#include <logos_mode.h>

#include <string>

// Deadlines for the calls that leave the machine, walk a whole package, or stop
// modules. The transport default (Timeout(), 20 s) fits a status query, not a
// 150 MB module.
// Both legs draw from here: the client's wait on the daemon, and the daemon's
// own wait on the package modules, which used to run on the default.
namespace rpc_deadlines {

constexpr int kCatalogMs  = 2  * 60 * 1000;   // fetch and resolve against the catalog
constexpr int kTransferMs = 30 * 60 * 1000;   // download, inspect, or install a package
// Unload stops modules one at a time, dependents first, and each may take up to
// 14 s: the container's 12 s stop window plus its 2 s SIGKILL wait.
constexpr int kUnloadMs   = 2  * 60 * 1000;

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
