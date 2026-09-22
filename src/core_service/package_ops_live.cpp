// Wires package_ops to the daemon: package modules over plain RPC, and the
// module runtime through logos_core. The logic is in package_ops.cpp.
#include "package_ops.h"
#include "logos_core.h"
#include "rpc_deadlines.h"
#include "../plain_rpc.h"

namespace package_ops {
namespace {

// The deadline is per method (rpc_deadlines.h): the package modules do the
// whole download or archive walk inside the call, and the transport default is 20 s.
Backend liveBackend(logosctl::PlainRpcContext* api)
{
    Backend b;
    b.call = [api](const char* module, const std::string& method,
                   const LogosList& args, std::string* why) -> nlohmann::json {
        if (!api) return nullptr;
        logosctl::PlainRpcClient* client = api->client(module);
        if (!client) {
            if (why) *why = std::string(module) + " is not reachable";
            return nullptr;
        }
        logosctl::PlainRpcError err;
        const nlohmann::json ret = client->invoke(
            method, args, rpc_deadlines::forPackageCall(method), &err);
        if (!err.ok()) {
            if (why) *why = err.message;
            return nullptr;
        }
        return ret;
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
    b.dependents = [](const std::string& m) {
        std::vector<std::string> out;
        char** deps = logos_core_get_module_dependents(m.c_str(), /*recursive=*/true);
        if (!deps) return out;
        for (int i = 0; deps[i]; ++i) {
            out.emplace_back(deps[i]);
            delete[] deps[i];
        }
        delete[] deps;
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

LogosMap plan(logosctl::PlainRpcContext* api, Op op, const std::vector<std::string>& names, const Options& opts)
{
    Backend b = liveBackend(api);
    return plan(b, op, names, opts);
}

LogosMap apply(logosctl::PlainRpcContext* api, Op op, const std::vector<std::string>& names, const Options& opts)
{
    Backend b = liveBackend(api);
    return apply(b, op, names, opts);
}

LogosMap download(logosctl::PlainRpcContext* api, const std::string& name, const Options& opts,
                  const std::string& destDir)
{
    Backend b = liveBackend(api);
    return download(b, name, opts, destDir);
}

} // namespace package_ops
