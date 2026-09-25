#include "package_service.h"
#include "package_ops.h"

#include <optional>
#include <vector>

namespace {

package_ops::Options toOptions(const LogosMap& opts)
{
    package_ops::Options o;
    o.withDeps       = opts.value("withDeps", true);
    o.withDependents = opts.value("withDependents", true);
    o.version        = opts.value("version", std::string{});
    o.rootHash       = opts.value("rootHash", std::string{});
    o.catalog        = opts.value("catalog", std::string{});
    if (opts.contains("localFiles") && opts["localFiles"].is_array()) {
        for (const auto& f : opts["localFiles"])
            o.localFiles.push_back(f.get<std::string>());
    }
    return o;
}

std::optional<package_ops::Op> toOp(const std::string& op)
{
    if (op == "install") return package_ops::Op::Install;
    if (op == "upgrade") return package_ops::Op::Upgrade;
    if (op == "remove")  return package_ops::Op::Remove;
    return std::nullopt;
}

std::vector<std::string> toNames(const LogosList& names)
{
    std::vector<std::string> out;
    if (names.is_array())
        for (const auto& n : names) out.push_back(n.get<std::string>());
    return out;
}

LogosMap unknownOperation(const std::string& op)
{
    return LogosMap{{"status", "error"}, {"code", "INVALID_ARGS"},
                    {"message", "Unknown package operation: " + op}};
}

} // namespace

LogosMap PackageService::planPackageOperation(const std::string& op, const LogosList& names,
                                              const LogosMap& opts)
{
    auto parsed = toOp(op);
    if (!parsed) return unknownOperation(op);
    return package_ops::plan(&m_rpc, *parsed, toNames(names), toOptions(opts));
}

LogosMap PackageService::applyPackageOperation(const std::string& op, const LogosList& names,
                                               const LogosMap& opts)
{
    auto parsed = toOp(op);
    if (!parsed) return unknownOperation(op);
    return package_ops::apply(&m_rpc, *parsed, toNames(names), toOptions(opts));
}

LogosMap PackageService::downloadPackage(const std::string& name, const LogosMap& opts)
{
    const std::string dest = opts.is_object() ? opts.value("output", std::string{}) : std::string{};
    return package_ops::download(&m_rpc, name, toOptions(opts), dest);
}

std::optional<nlohmann::json> PackageService::call(const std::string& method,
                                                   const nlohmann::json& args)
{
    try {
        if (method == "planPackageOperation" && args.size() >= 3)
            return planPackageOperation(args[0].get<std::string>(), args[1], args[2]);
        if (method == "applyPackageOperation" && args.size() >= 3)
            return applyPackageOperation(args[0].get<std::string>(), args[1], args[2]);
        if (method == "downloadPackage" && args.size() >= 2)
            return downloadPackage(args[0].get<std::string>(), args[1]);
    } catch (const std::exception& e) {
        return nlohmann::json{{"status", "error"}, {"code", "INVALID_ARGS"},
                              {"message", std::string("invalid arguments: ") + e.what()}};
    }
    return std::nullopt;
}

bool PackageService::owns(const std::string& method)
{
    for (const auto& entry : methods())
        if (entry.value("name", std::string{}) == method) return true;
    return false;
}

nlohmann::json PackageService::methods()
{
    auto param = [](const char* name, const char* type) {
        return nlohmann::json{{"name", name}, {"type", type}};
    };
    auto method = [](const char* name, nlohmann::json params) {
        return nlohmann::json{{"type", "method"}, {"name", name}, {"returnType", "LogosMap"},
                              {"isInvokable", true}, {"parameters", std::move(params)}};
    };
    return nlohmann::json::array({
        method("planPackageOperation",
               {param("op", "string"), param("names", "LogosList"), param("opts", "LogosMap")}),
        method("applyPackageOperation",
               {param("op", "string"), param("names", "LogosList"), param("opts", "LogosMap")}),
        method("downloadPackage", {param("name", "string"), param("opts", "LogosMap")}),
    });
}
