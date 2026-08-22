#ifndef CORE_SERVICE_CALL_ENVELOPE_H
#define CORE_SERVICE_CALL_ENVELOPE_H

// How a proxied module call becomes the `logosctl call` JSON envelope.
//
// Split out of core_service_impl.cpp because it is the part that was WRONG and
// the part worth pinning: given what the transport reported and what the module
// answered, decide ok / METHOD_FAILED / METHOD_NOT_FOUND. Nothing here touches
// Qt, logos-protocol or a live transport, so the whole decision is exercised by
// the unit suite (tests/test_call_envelope.cpp) instead of only by a
// daemon-backed integration run.

#include <logos_json.h>

#include <functional>
#include <string>
#include <vector>

namespace core_service {

// {code, message, origin}; an empty code means "no error".
//
// The std mirror of logos::CallError (logos-protocol/cpp/logos_call_error.h) —
// deliberately a separate type so this header stays free of the protocol
// include, which the unit-test library does not link. core_service_impl.cpp
// copies the three fields across at the one call site.
struct CallFailure {
    std::string code;
    std::string message;
    std::string origin;

    bool ok() const { return code.empty(); }
};

// True when `v` is the canonical provider REJECTION object rather than a value;
// fills `out` with its {code, message, origin} on a match.
//
// A provider that RAN and refused the call answers
// {"code":"dispatch_failed", "message":..., "origin":...} as its RESULT, not
// through the transport's error channel — logos_protocol.h states that split
// explicitly. Generated typed consumers fold it into logos::CallError
// themselves (emitDispatchRejectionDetectorJson in logos-cpp-sdk's
// generator_lib.cpp, and lidl_gen_qt_consumer.cpp's byte-identical twin);
// `logosctl call` is the UNTYPED consumer of that same surface, so it has to
// fold it here or a refusal would read as a successful call that returned a
// three-key map.
//
// The match is exact — those three fields, all strings, and that code — for the
// same reason the generated detector is exact: a map return carrying user data
// must never false-match.
bool dispatchRejection(const nlohmann::json& v, CallFailure& out);

// Supplies the module's exposed method names, or an empty vector when the
// module could not be asked. callEnvelope invokes this AT MOST ONCE, and only
// on the single path that needs it, so the ordinary call still costs exactly
// one round-trip.
using MethodLister = std::function<std::vector<std::string>()>;

// The envelope for one proxied call.
//
//   {"status":"ok","module":...,"method":...,"result":<value>}
//   {"status":"error","code":"METHOD_FAILED","message":...,"error":{...}}
//   {"status":"error","code":"METHOD_NOT_FOUND","message":...,
//    "available_methods":[...]}
//
// `failure` is what the transport reported (see CallFailure); `ret` is the
// value the module answered with, already decoded to JSON.
LogosMap callEnvelope(const std::string& module,
                      const std::string& method,
                      const nlohmann::json& ret,
                      CallFailure failure,
                      const MethodLister& listMethods);

} // namespace core_service

#endif // CORE_SERVICE_CALL_ENVELOPE_H
