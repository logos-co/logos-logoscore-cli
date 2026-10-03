#ifndef PACKAGE_SERVICE_H
#define PACKAGE_SERVICE_H

// The daemon's own methods on core_service, which liblogos runs: package
// operations, reached through logos_core_set_core_service_extension.

#include "../plain_rpc.h"

#include <logos_json.h>
#include <optional>
#include <string>

class PackageService
{
public:
    LogosMap planPackageOperation(const std::string& op, const LogosList& names,
                                  const LogosMap& opts);
    LogosMap applyPackageOperation(const std::string& op, const LogosList& names,
                                   const LogosMap& opts);
    // Fetches a .lgx without installing it, into the requested directory.
    LogosMap downloadPackage(const std::string& name, const LogosMap& opts);

    // Nothing for a method that is not one of these, or for the wrong arguments.
    std::optional<nlohmann::json> call(const std::string& method, const nlohmann::json& args);
    static nlohmann::json methods();
    static bool owns(const std::string& method);

private:
    // The package modules take their settings from the runtime only.
    logosctl::PlainRpcContext m_rpc{"core"};
};

#endif
