// The proxied-call envelope: ok vs METHOD_FAILED vs METHOD_NOT_FOUND.
//
// The whole point of this file is ONE distinction. `logosctl call` used to
// report METHOD_FAILED whenever the module answered null, because
// core_service_impl.cpp reached for the invokeRemoteMethod overload that has no
// logos::CallError* parameter and therefore had nothing but the value to judge
// by. A method that legitimately returns nothing was indistinguishable from a
// call that never ran.
//
// The error channel already carried that distinction everywhere else — lp_invoke
// branches on callErr.ok() and never on the value — so these tests pin the two
// answers apart at the layer that used to conflate them:
//
//   transport reported a failure        -> METHOD_FAILED   (+ error.code)
//   provider RAN and refused            -> METHOD_FAILED   (+ its rejection code)
//   module has no such method           -> METHOD_NOT_FOUND (+ available_methods)
//   method exists and answered null     -> ok, result null   <- the change
//
// No Qt, no transport, no daemon: callEnvelope is pure, and the introspection
// hop it needs for the third case arrives as a callback these tests supply.

#include <gtest/gtest.h>

#include "core_service/call_envelope.h"

using core_service::CallFailure;
using core_service::callEnvelope;
using core_service::dispatchRejection;

namespace {

// An introspection hook that records whether it was consulted, so the tests can
// assert the ordinary path does NOT pay for the extra round-trip.
struct Lister {
    std::vector<std::string> names;
    mutable int calls = 0;

    core_service::MethodLister fn() const {
        return [this]() { ++calls; return names; };
    }
};

const Lister kBasicModule{{"returnTrue", "returnNothing", "echo"}};

} // namespace

// ── The behaviour change: null is a VALUE, not a failure ────────────────────

TEST(CallEnvelope, NullFromAKnownMethodIsOk)
{
    const LogosMap env = callEnvelope("test_basic_module", "returnNothing",
                                      nlohmann::json(), CallFailure{},
                                      kBasicModule.fn());

    EXPECT_EQ(env.value("status", std::string{}), "ok");
    EXPECT_EQ(env.value("module", std::string{}), "test_basic_module");
    EXPECT_EQ(env.value("method", std::string{}), "returnNothing");
    ASSERT_TRUE(env.contains("result"));
    EXPECT_TRUE(env["result"].is_null())
        << "a null return must survive as a null RESULT, not become an error";
    EXPECT_FALSE(env.contains("code"));
}

TEST(CallEnvelope, FalseyValuesAreOkToo)
{
    // These all used to be safe (only `null` tripped the old check), but they
    // are the neighbours of the case that broke, so pin them.
    for (const nlohmann::json v : {nlohmann::json(false), nlohmann::json(0),
                                   nlohmann::json(""), nlohmann::json::array(),
                                   nlohmann::json::object()}) {
        const LogosMap env = callEnvelope("m", "echo", v, CallFailure{},
                                          kBasicModule.fn());
        EXPECT_EQ(env.value("status", std::string{}), "ok") << v.dump();
        EXPECT_EQ(env.value("result", nlohmann::json()), v) << v.dump();
    }
}

TEST(CallEnvelope, OrdinaryCallNeverIntrospects)
{
    Lister lister{{"echo"}};
    callEnvelope("m", "echo", nlohmann::json("hi"), CallFailure{}, lister.fn());
    EXPECT_EQ(lister.calls, 0)
        << "a non-null result must cost exactly one round-trip";
}

// ── A genuinely failed call still reports METHOD_FAILED ─────────────────────

TEST(CallEnvelope, TransportFailureIsMethodFailed)
{
    // Every code logos::CallError can carry. All of them are failures of the
    // call, and all of them keep the single documented METHOD_FAILED code.
    for (const char* code : {"object_unavailable", "timeout", "transport_error",
                             "call_failed", "unauthorized"}) {
        Lister lister{{"returnTrue"}};
        const LogosMap env = callEnvelope(
            "test_basic_module", "returnTrue", nlohmann::json(),
            CallFailure{code, "the transport said so", "test_basic_module"},
            lister.fn());

        EXPECT_EQ(env.value("status", std::string{}), "error") << code;
        EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED") << code;
        // The specific failure stays machine-readable rather than being
        // flattened into prose.
        ASSERT_TRUE(env.contains("error")) << code;
        EXPECT_EQ(env["error"].value("code", std::string{}), code);
        EXPECT_EQ(env["error"].value("origin", std::string{}), "test_basic_module");
        EXPECT_NE(env.value("message", std::string{}).find(code), std::string::npos)
            << "the human message should name the underlying failure";
        EXPECT_EQ(lister.calls, 0)
            << "a failed call must not go back to a module that just failed";
    }
}

TEST(CallEnvelope, FailureWinsOverAValue)
{
    // A transport failure can still hand back a value; the error channel is
    // what decides, in both directions.
    const LogosMap env = callEnvelope("m", "echo", nlohmann::json("leftover"),
                                      CallFailure{"timeout", "took too long", "m"},
                                      kBasicModule.fn());
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
}

// ── A provider that RAN and refused ─────────────────────────────────────────

TEST(CallEnvelope, DispatchRejectionIsMethodFailed)
{
    const nlohmann::json refusal{{"code", "dispatch_failed"},
                                 {"message", "wrong argument count"},
                                 {"origin", "test_basic_module"}};

    const LogosMap env = callEnvelope("test_basic_module", "isPositive", refusal,
                                      CallFailure{}, kBasicModule.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
    ASSERT_TRUE(env.contains("error"));
    EXPECT_EQ(env["error"].value("code", std::string{}), "dispatch_failed");
    EXPECT_EQ(env["error"].value("message", std::string{}), "wrong argument count");
}

// The LIVE bug this widening fixes. A provider answers an arity error with
// {"code":"invalid_args", ...} as its RESULT — logos-cpp-sdk's cdylib dispatch
// and logos-rust-sdk's args::invalid_args both do, and have all along. Before
// the detector matched a closed SET rather than the single literal
// "dispatch_failed", this envelope came back status "ok" with the refusal as
// the value, and `logosctl call` exited 0. Measured, on
// `logosctl call test_basic_module isPositive` with the argument missing.
TEST(CallEnvelope, InvalidArgsIsMethodFailed)
{
    const nlohmann::json refusal{{"code", "invalid_args"},
                                 {"message", "expected 1 arguments, got 0"},
                                 {"origin", "test_basic_module"}};

    const LogosMap env = callEnvelope("test_basic_module", "isPositive", refusal,
                                      CallFailure{}, kBasicModule.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED");
    ASSERT_TRUE(env.contains("error"));
    EXPECT_EQ(env["error"].value("code", std::string{}), "invalid_args");
    EXPECT_EQ(env["error"].value("message", std::string{}),
              "expected 1 arguments, got 0");
    // The refusal must NOT also survive as a value: an envelope carrying both
    // would let a caller keep reading it as data.
    EXPECT_FALSE(env.contains("result"));
}

// Every code in the closed set folds, not just dispatch_failed. "unknown_method"
// is here before any provider emits it — that readiness is the point of doing
// the detectors first.
TEST(CallEnvelope, EveryRejectionCodeIsMethodFailed)
{
    for (const char* code : {"dispatch_failed", "invalid_args", "unknown_method"}) {
        CallFailure out;
        EXPECT_TRUE(dispatchRejection(nlohmann::json{{"code", code},
                                                     {"message", "m"},
                                                     {"origin", "o"}}, out))
            << code;
        EXPECT_EQ(out.code, code);
        EXPECT_EQ(out.message, "m");
        EXPECT_EQ(out.origin, "o");

        const LogosMap env = callEnvelope("test_basic_module", "isPositive",
                                          nlohmann::json{{"code", code},
                                                         {"message", "m"},
                                                         {"origin", "o"}},
                                          CallFailure{}, kBasicModule.fn());
        EXPECT_EQ(env.value("code", std::string{}), "METHOD_FAILED") << code;
    }
}

// The NEGATIVES. Widening one literal into a set is one careless edit away from
// "any object with a code", which would hand every method that legitimately
// returns a three-string map to the error channel. These are what keep the
// match closed.
TEST(CallEnvelope, DispatchRejectionMatchStaysNarrow)
{
    CallFailure out;
    // Right shape, right code.
    EXPECT_TRUE(dispatchRejection(nlohmann::json{{"code", "dispatch_failed"},
                                                 {"message", "m"},
                                                 {"origin", "o"}}, out));

    // A code OUTSIDE the closed set stays DATA, however plausible. This is the
    // difference between a closed set and an open shape match.
    for (const char* code : {"", "ok", "not_found", "user_error",
                             "DISPATCH_FAILED", "dispatch_failed ",
                             "invalid_argument", "unknown_methods"}) {
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"},
                                                      {"origin", "o"}}, out))
            << code;
    }

    // Wrong key COUNT, for every code in the set — 2 keys and 4 keys.
    for (const char* code : {"dispatch_failed", "invalid_args", "unknown_method"}) {
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"}}, out))
            << code;
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"},
                                                      {"origin", "o"},
                                                      {"extra", 1}}, out))
            << code;
        // A non-string in any of the three slots.
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", 7},
                                                      {"message", "m"},
                                                      {"origin", "o"}}, out))
            << code;
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", 7},
                                                      {"origin", "o"}}, out))
            << code;
        EXPECT_FALSE(dispatchRejection(nlohmann::json{{"code", code},
                                                      {"message", "m"},
                                                      {"origin", nullptr}}, out))
            << code;
    }

    EXPECT_FALSE(dispatchRejection(nlohmann::json::array(), out));
    EXPECT_FALSE(dispatchRejection(nlohmann::json(), out));

    // And end-to-end: an unrecognised three-string map is a RESULT, not an error.
    const nlohmann::json userMap{{"code", "amber"},
                                 {"message", "hello"},
                                 {"origin", "sensor"}};
    const LogosMap env = callEnvelope("test_basic_module", "describe", userMap,
                                      CallFailure{}, kBasicModule.fn());
    EXPECT_EQ(env.value("status", std::string{}), "ok");
    EXPECT_EQ(env["result"], userMap);
}

// ── An unknown method: the one case the wire cannot report ──────────────────

TEST(CallEnvelope, UnknownMethodIsMethodNotFound)
{
    // Providers answer an unknown method with a bare null and no error — see
    // logos_protocol.h and lidl_gen_cdylib.cpp's `return nullptr; // unknown
    // method`. Only the module's own method list can settle it.
    Lister lister{{"returnTrue", "echo"}};
    const LogosMap env = callEnvelope("test_basic_module", "noSuchMethod",
                                      nlohmann::json(), CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("status", std::string{}), "error");
    EXPECT_EQ(env.value("code", std::string{}), "METHOD_NOT_FOUND");
    EXPECT_EQ(env.value("message", std::string{}),
              "Method 'noSuchMethod' not found on module 'test_basic_module'.");
    ASSERT_TRUE(env.contains("available_methods"));
    EXPECT_EQ(env["available_methods"],
              nlohmann::json::array({"returnTrue", "echo"}));
    EXPECT_EQ(lister.calls, 1) << "introspection must be consulted exactly once";
}

TEST(CallEnvelope, UnprovableMissingMethodStaysOk)
{
    // Introspection unavailable (module wedged, transport dropped, provider
    // that publishes nothing). We cannot prove the method is missing, so we do
    // not claim it — that would be the old null-means-failure guess wearing a
    // better name.
    Lister lister{{}};
    const LogosMap env = callEnvelope("m", "whoKnows", nlohmann::json(),
                                      CallFailure{}, lister.fn());

    EXPECT_EQ(env.value("status", std::string{}), "ok");
    ASSERT_TRUE(env.contains("result"));
    EXPECT_TRUE(env["result"].is_null());
    EXPECT_EQ(lister.calls, 1);
}

TEST(CallEnvelope, NoListerAtAllStaysOk)
{
    const LogosMap env = callEnvelope("m", "whoKnows", nlohmann::json(),
                                      CallFailure{}, nullptr);
    EXPECT_EQ(env.value("status", std::string{}), "ok");
}

// ── The distinction, stated as one assertion ────────────────────────────────

TEST(CallEnvelope, EmptyAndFailedAreDistinguishable)
{
    const LogosMap empty = callEnvelope("m", "returnNothing", nlohmann::json(),
                                        CallFailure{}, kBasicModule.fn());
    const LogosMap failed = callEnvelope(
        "m", "returnNothing", nlohmann::json(),
        CallFailure{"object_unavailable", "module is gone", "m"},
        kBasicModule.fn());

    // Same method, same null on the wire, opposite answers — which is the
    // entire point of reading the error channel.
    EXPECT_NE(empty.value("status", std::string{}),
              failed.value("status", std::string{}));
    EXPECT_EQ(empty.value("status", std::string{}), "ok");
    EXPECT_EQ(failed.value("code", std::string{}), "METHOD_FAILED");
}
