// Wires package_ops to the daemon: package modules over plain RPC, and the
// module runtime through core_service as the daemon's shell (shell_calls).
// The logic is in package_ops.cpp.
#include "package_ops.h"
#include "shell_calls.h"
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
    b.loadedModules = [] { return shell_calls::loaded(); };
    b.dependents = [](const std::string& m) { return shell_calls::dependents(m); };
    b.unloadModule = [](const std::string& m) { shell_calls::unload(m, /*withDependents=*/true); };
    b.loadModule = [](const std::string& m) { return shell_calls::load(m); };
    b.refreshModules = [] { shell_calls::refresh(); };
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
