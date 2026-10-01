// Wires package_ops to the daemon: package modules over LogosAPI, and the
// module runtime through logos_core. The logic is in package_ops.cpp.
#include "package_ops.h"
#include "logos_core.h"
#include "rpc_deadlines.h"

#include <logos_api.h>
#include <logos_api_client.h>
#include <logos_call_error.h>
#include <logos_json_convert.h>

namespace package_ops {
namespace {

// The deadline is per method (rpc_deadlines.h): the package modules do the
// whole download or archive walk inside the call, and the transport default is 20 s.
Backend liveBackend(LogosAPI* api)
{
    Backend b;
    b.call = [api](const char* module, const std::string& method,
                   const LogosList& args, std::string* why) -> nlohmann::json {
        if (!api) return nullptr;
        LogosAPIClient* client = api->getClient(module);
        if (!client) {
            if (why) *why = std::string(module) + " is not reachable";
            return nullptr;
        }
        logos::CallError err;
        const QVariant ret = client->invokeRemoteMethod(
            QString::fromStdString(module), QString::fromStdString(method),
            logos::nlohmannArgsToQVariantList(args),
            rpc_deadlines::forPackageCall(method), &err);
        if (!err.ok()) {
            if (why) *why = err.message;
            return nullptr;
        }
        return logos::qvariantToNlohmann(ret);
    };
    b.loadedModules = [] {
        std::vector<std::string> out;
        char** mods = logos_core_get_loaded_modules();
        if (!mods) return out;
        for (int i = 0; mods[i]; ++i) {
            out.emplace_back(mods[i]);
            delete[] mods[i];
        }
        delete[] mods;
        return out;
    };
    b.unloadModule = [](const std::string& m) {
        logos_core_unload_module(m.c_str(), /*with_dependents=*/true);
    };
    b.loadModule = [](const std::string& m) {
        return logos_core_load_module(m.c_str(), LOGOS_LOAD_REQUIRED_AND_OPTIONAL) != 0;
    };
    b.refreshModules = [] { logos_core_refresh_modules(); };
    return b;
}

} // namespace

LogosMap plan(LogosAPI* api, Op op, const std::vector<std::string>& names, const Options& opts)
{
    Backend b = liveBackend(api);
    return plan(b, op, names, opts);
}

LogosMap apply(LogosAPI* api, Op op, const std::vector<std::string>& names, const Options& opts)
{
    Backend b = liveBackend(api);
    return apply(b, op, names, opts);
}

LogosMap download(LogosAPI* api, const std::string& name, const Options& opts,
                  const std::string& destDir)
{
    Backend b = liveBackend(api);
    return download(b, name, opts, destDir);
}

} // namespace package_ops
