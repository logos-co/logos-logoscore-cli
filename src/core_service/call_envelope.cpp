#include "call_envelope.h"

#include <algorithm>

namespace core_service {

bool dispatchRejection(const nlohmann::json& v, CallFailure& out)
{
    if (!v.is_object() || v.size() != 3) return false;
    auto code = v.find("code"), message = v.find("message"), origin = v.find("origin");
    if (code == v.end() || message == v.end() || origin == v.end()) return false;
    if (!code->is_string() || !message->is_string() || !origin->is_string()) return false;
    if (code->get<std::string>() != "dispatch_failed") return false;
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

    if (!failure.ok()) {
        // ONE code for every transport-detected failure, exactly as before:
        // object_unavailable / timeout / transport_error / call_failed /
        // unauthorized, plus the folded dispatch_failed. The specific code
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

    // The one ambiguity the wire really does have.
    //
    // Every provider flavour answers an unknown method name with a bare null,
    // byte-identical to a method that legitimately returns null.
    // logos_protocol.h says so in as many words ("NOT reported, and it is not
    // an oversight: an unknown method name"), and the cdylib dispatch ends in
    // `return nullptr;  // unknown method` (lidl_gen_cdylib.cpp). No transport
    // can separate the two — but core_service can ASK, because the module
    // publishes its own method list. That happens only on a null return, so
    // the ordinary path is unaffected.
    //
    // Stay silent when introspection fails or comes back empty: an unproven
    // METHOD_NOT_FOUND would just be the old null-means-failure guess wearing a
    // better name.
    if (ret.is_null() && listMethods) {
        const std::vector<std::string> names = listMethods();
        if (!names.empty()
            && std::find(names.begin(), names.end(), method) == names.end()) {
            const std::string msg =
                "Method '" + method + "' not found on module '" + module + "'.";
            result["status"]            = "error";
            result["code"]              = "METHOD_NOT_FOUND";
            result["message"]           = msg;
            result["available_methods"] = names;   // docs/spec.md's envelope
            return result;
        }
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
