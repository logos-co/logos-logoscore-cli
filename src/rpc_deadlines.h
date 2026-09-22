#pragma once

#include <string>

// Deadlines for the calls that leave the machine or walk a whole package. The
// transport default (0 selects the protocol's 20 s default) fits a status
// query, not a 150 MB module.
// Both legs draw from here: the client's wait on the daemon, and the daemon's
// own wait on the package modules, which used to run on the default.
namespace rpc_deadlines {

constexpr int kCatalogMs  = 2  * 60 * 1000;   // fetch and resolve against the catalog
constexpr int kTransferMs = 30 * 60 * 1000;   // download, inspect, or install a package

// The daemon's deadline for one call into package_manager / package_downloader.
// The heavy methods do the entire transfer or archive walk inside the call.
inline int forPackageCall(const std::string& method)
{
    if (method == "downloadResolvedDependencies" || method == "downloadPinned"
        || method == "inspectPackage" || method == "installPlugin")
        return kTransferMs;
    if (method == "resolveDependencies")
        return kCatalogMs;
    return 0;
}

} // namespace rpc_deadlines
