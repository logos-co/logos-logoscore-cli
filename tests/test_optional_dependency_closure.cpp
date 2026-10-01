#include <gtest/gtest.h>
#include "core_service/optional_dependency_closure.h"

using nlohmann::json;
namespace {
json offer(const std::string& name) {
    return {{"name", name}, {"request", {{"name", name}, {"version", "0.10.0"},
        {"rootHash", name + "-artifact"}, {"signer", "did:jwk:publisher"}}}};
}
json row(const std::string& name) { return {{"name", name}, {"version", "0.10.0"}}; }
}

TEST(OptionalClosure, AllResolvesRlnRequiredChildAndNestedOptionals) {
    int calls = 0;
    const auto plan = package_ops::resolveOptionalClosure(json::array({"delivery_module"}), true,
        [&](const json& inputs) {
            ++calls;
            auto delivery = row("delivery_module");
            if (calls == 1) {
                EXPECT_EQ(inputs.size(), 1u);
                auto missing = offer("unavailable"); missing["error"] = "not available";
                delivery["optionalDependencies"] = json::array({offer("rln"), missing});
                return json::array({delivery});
            }
            EXPECT_EQ(inputs[1]["name"], "rln");
            EXPECT_EQ(inputs[1]["rootHash"], "rln-artifact");
            EXPECT_EQ(inputs[1]["signer"], "did:jwk:publisher");
            delivery["dependencyGraph"] = {{"delivery_module", json::array()},
                {"rln", json::array({"lez_rln"})}, {"lez_rln", json::array()}};
            if (calls == 2) {
                EXPECT_EQ(inputs.size(), 2u);
                delivery["optionalDependencies"] = json::array({offer("nested")});
                return json::array({row("lez_rln"), row("rln"), delivery});
            }
            EXPECT_EQ(inputs.size(), 3u);
            EXPECT_EQ(inputs[2]["name"], "nested");
            return json::array({row("nested"), row("lez_rln"), row("rln"), delivery});
        });
    EXPECT_EQ(calls, 3);
    ASSERT_EQ(plan.size(), 4u);
    EXPECT_EQ(plan[1]["name"], "lez_rln");
    EXPECT_FALSE(plan[1]["optional"].get<bool>());
    EXPECT_TRUE(plan[2]["optional"].get<bool>());
    EXPECT_TRUE(plan[0]["optional"].get<bool>());
    EXPECT_FALSE(plan[3]["optional"].get<bool>());
}

TEST(OptionalClosure, MandatoryOnlyResolvesOnceAndExcludesOptionalBranches) {
    int calls = 0;
    auto delivery = row("delivery_module");
    delivery["optionalDependencies"] = json::array({offer("rln")});
    const auto plan = package_ops::resolveOptionalClosure(json::array({"delivery_module"}), false,
        [&](const json& inputs) { ++calls; EXPECT_EQ(inputs.size(), 1u); return json::array({delivery}); });
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0]["name"], "delivery_module");
}

TEST(OptionalClosure, CyclesAndRepeatedOffersNeverDuplicateInputs) {
    int calls = 0;
    auto delivery = row("delivery_module");
    delivery["optionalDependencies"] = json::array({offer("rln"), offer("rln"), offer("delivery_module")});
    const auto plan = package_ops::resolveOptionalClosure(json::array({"delivery_module"}), true,
        [&](const json& inputs) {
            ++calls;
            EXPECT_LE(inputs.size(), 2u);
            return inputs.size() == 1 ? json::array({delivery}) : json::array({row("rln"), delivery});
        });
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(plan.size(), 2u);
}

TEST(OptionalClosure, ResolverErrorsStopFurtherExpansion) {
    int calls = 0;
    const auto plan = package_ops::resolveOptionalClosure(json::array({"delivery_module"}), true,
        [&](const json&) { ++calls; return json::array({json{{"name", "rln"}, {"error", "missing required child"}}}); });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(plan[0]["error"], "missing required child");
}

TEST(OptionalClosure, OfferThatIsARequiredChildOfAnotherSelectionIsNotPinned) {
    // S offers Y and Z; Z requires Y. Y must install as Z's dependency, not as a pinned root.
    int calls = 0;
    json last;
    const auto plan = package_ops::resolveOptionalClosure(json::array({"S"}), true,
        [&](const json& inputs) {
            ++calls;
            last = inputs;
            auto s = row("S");
            s["optionalDependencies"] = json::array({offer("Y"), offer("Z")});
            bool hasZ = false;
            for (const auto& i : inputs) if (i.is_object() && i["name"] == "Z") hasZ = true;
            if (hasZ) s["dependencyGraph"] = {{"S", json::array()}, {"Z", json::array({"Y"})}};
            return hasZ ? json::array({row("Y"), row("Z"), s}) : json::array({s});
        });
    ASSERT_EQ(last.size(), 2u);
    EXPECT_EQ(last[1]["name"], "Z");
    EXPECT_FALSE(plan[0]["optional"].get<bool>());
    EXPECT_TRUE(plan[1]["optional"].get<bool>());
    EXPECT_EQ(calls, 3);
}

TEST(OptionalClosure, MalformedGraphEntriesAreIgnored) {
    const auto plan = package_ops::resolveOptionalClosure(json::array({"S"}), true,
        [&](const json&) {
            auto s = row("S");
            s["dependencyGraph"] = {{"S", json::array({nullptr, 3, "A"})}};
            return json::array({s});
        });
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_FALSE(plan[0]["optional"].get<bool>());
}

TEST(OptionalClosure, InstalledOptionalAtItsInstalledReleaseIsKept) {
    int calls = 0;
    auto installed = offer("rln");
    installed["version"] = "0.10.0";
    installed["installedVersion"] = "0.10.0";
    auto s = row("S");
    s["optionalDependencies"] = json::array({installed});
    const auto plan = package_ops::resolveOptionalClosure(json::array({"S"}), true,
        [&](const json& inputs) { ++calls; EXPECT_EQ(inputs.size(), 1u); return json::array({s}); });
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(plan.size(), 1u);
}

TEST(OptionalClosure, NewOptionalsCanStayUnselected) {
    int calls = 0;
    auto s = row("S");
    s["optionalDependencies"] = json::array({offer("rln")});
    const auto plan = package_ops::resolveOptionalClosure(json::array({"S"}), true,
        [&](const json& inputs) { ++calls; EXPECT_EQ(inputs.size(), 1u); return json::array({s}); },
        /*selectNew=*/false);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(plan.size(), 1u);
}
