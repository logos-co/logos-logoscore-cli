#include "call_envelope.h"

#include <algorithm>

namespace core_service {
namespace {

// The CLOSED SET of provider-refusal codes, in one place so it cannot drift
// against the rest of the function. See call_envelope.h for why the set is
// closed and which envelope each code becomes.
const char* const kRejectionCodes[] = {
    "dispatch_failed", "invalid_args", "unknown_method",
};

bool isRejectionCode(const std::string& c)
{
    for (const char* k : kRejectionCodes)
        if (c == k) return true;
    return false;
}

LogosMap methodNotFound(const std::string& module, const std::string& method,
                        const std::vector<std::string>& names)
{
    LogosMap result;
    result["status"]            = "error";
    result["code"]              = "METHOD_NOT_FOUND";
    result["message"]           = "Method '" + method + "' not found on module '" + module + "'.";
    result["available_methods"] = names;   // docs/spec.md's envelope
    return result;
}

} // namespace

bool dispatchRejection(const nlohmann::json& v, CallFailure& out)
{
    if (!v.is_object() || v.size() != 3) return false;
    auto code = v.find("code"), message = v.find("message"), origin = v.find("origin");
    if (code == v.end() || message == v.end() || origin == v.end()) return false;
    if (!code->is_string() || !message->is_string() || !origin->is_string()) return false;
    if (!isRejectionCode(code->get<std::string>())) return false;
    out.code    = code->get<std::string>();
    out.message = message->get<std::string>();
    out.origin  = origin->get<std::string>();
    return true;
}

LogosMap callEnvelope(const std::string& module,
                      const std::string& method,
                      const nlohmann::json& ret,
                      CallFailure failure,
                      const MethodLister& listMethods)
{
    LogosMap result;

    // A provider that ran and REFUSED answers through the result rather than
    // the error channel, so fold that in before deciding: both are failures of
    // the call and must read identically to whoever asked.
    if (failure.ok()) dispatchRejection(ret, failure);

    // The provider refused the NAME: same envelope as the null-return rescue below.
    if (failure.code == "unknown_method")
        return methodNotFound(module, method,
                              listMethods ? listMethods() : std::vector<std::string>{});

    if (!failure.ok()) {
        // ONE code for every transport-detected failure, exactly as before:
        // object_unavailable / timeout / transport_error / call_failed /
        // unauthorized, plus the folded provider refusal. The specific code
        // rides in `error` so a JSON consumer can tell them apart without
        // parsing prose, and is appended to the message for a human reader.
        const std::string msg = "Call to " + module + "." + method + " failed ("
                              + failure.code + ": " + failure.message + ").";
        result["status"]  = "error";
        result["code"]    = "METHOD_FAILED";
        result["message"] = msg;
        result["error"]   = LogosMap{{"code",    failure.code},
                                     {"message", failure.message},
                                     {"origin",  failure.origin}};
        return result;
    }

    // A module built before providers refused unknown names answers one with a
    // bare null, byte-identical to a method that legitimately returns null. No
    // transport can separate the two — but core_service can ASK, because the
    // module publishes its own method list. That happens only on a null return,
    // so the ordinary path is unaffected.
    //
    // Stay silent when introspection fails or comes back empty: an unproven
    // METHOD_NOT_FOUND would just be the old null-means-failure guess wearing a
    // better name.
    if (ret.is_null() && listMethods) {
        const std::vector<std::string> names = listMethods();
        if (!names.empty()
            && std::find(names.begin(), names.end(), method) == names.end())
            return methodNotFound(module, method, names);
    }

    // Success — INCLUDING a null result. `null` is a value here: an empty
    // optional, or a method that returns nothing in particular. It stopped
    // meaning "the call failed" when this function started reading the error
    // channel instead of the value.
    result["status"] = "ok";
    result["module"] = module;
    result["method"] = method;
    result["result"] = ret;
    return result;
}

} // namespace core_service
