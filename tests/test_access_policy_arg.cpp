// =============================================================================
// Tests for the --access-policy argument resolver
// (src/daemon/access_policy_arg.{h,cpp}).
//
// This is the operator-facing half of deny-by-default enforcement: whatever
// this returns is handed verbatim to logos_core_set_access_policy(), where
// `mode: "enforce"` (and only that) arms the runtime. The tests pin:
//   - `enforce` expands to a document the runtime reads as enforce mode
//   - the alias wins over the file branch (no relative path named "enforce")
//   - inline JSON and file paths are passed through unchanged
//   - a bad path / malformed JSON fails with a reason instead of silently
//     degrading to "no policy" (which would look exactly like flag-off)
// =============================================================================
#include <gtest/gtest.h>

#include "daemon/access_policy_arg.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using logoscore::resolveAccessPolicyArg;

namespace {

// Scratch directory that cleans itself up, so the file-path cases don't leave
// droppings in the build sandbox.
class TempDir {
public:
    TempDir() {
        base = fs::temp_directory_path() /
               ("logoscore_ap_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(base);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(base, ec); }

    fs::path write(const std::string& name, const std::string& content) const {
        const fs::path p = base / name;
        std::ofstream(p) << content;
        return p;
    }

    fs::path base;
};

} // namespace

// ── The deny-by-default alias ────────────────────────────────────────────────

TEST(AccessPolicyArg, EnforceAliasExpandsToAnEnforcePolicy) {
    std::string err;
    auto resolved = resolveAccessPolicyArg("enforce", &err);
    ASSERT_TRUE(resolved.has_value()) << err;

    // The value only matters through the runtime's eyes: it must parse, and it
    // must carry mode "enforce" — that field is what flips deny-by-default on.
    const auto doc = nlohmann::json::parse(*resolved);
    EXPECT_EQ(doc.value("mode", std::string{}), "enforce");
    // No explicit restrictions: the runtime derives them from the declared
    // dependency graph, which is what "deny-by-default" means here.
    EXPECT_TRUE(doc.value("restrictions", nlohmann::json::object()).empty());
}

TEST(AccessPolicyArg, EnforceAliasIsNotTreatedAsAFilePath) {
    // Even with a readable file literally named `enforce` next to the process,
    // the alias must win — otherwise arming enforcement would depend on the
    // daemon's working directory.
    TempDir dir;
    dir.write("enforce", R"({"version":1,"mode":"audit"})");
    const fs::path prev = fs::current_path();
    fs::current_path(dir.base);

    std::string err;
    auto resolved = resolveAccessPolicyArg("enforce", &err);
    fs::current_path(prev);

    ASSERT_TRUE(resolved.has_value()) << err;
    EXPECT_EQ(nlohmann::json::parse(*resolved).value("mode", std::string{}), "enforce");
}

// ── Inline JSON ──────────────────────────────────────────────────────────────

TEST(AccessPolicyArg, InlineJsonPassesThroughUnchanged) {
    const std::string inlineDoc =
        R"({"version":1,"mode":"enforce","restrictions":{)"
        R"("package_manager":{"allowedCallers":["package_manager_ui"]}}})";
    std::string err;
    auto resolved = resolveAccessPolicyArg(inlineDoc, &err);
    ASSERT_TRUE(resolved.has_value()) << err;
    EXPECT_EQ(*resolved, inlineDoc);
}

TEST(AccessPolicyArg, LeadingWhitespaceStillCountsAsInline) {
    std::string err;
    auto resolved = resolveAccessPolicyArg("  \n {\"version\":1,\"mode\":\"enforce\"}", &err);
    ASSERT_TRUE(resolved.has_value()) << err;
    EXPECT_NE(resolved->find("enforce"), std::string::npos);
}

// ── File paths ───────────────────────────────────────────────────────────────

TEST(AccessPolicyArg, FilePathIsReadFromDisk) {
    TempDir dir;
    const std::string doc =
        R"({"version":1,"mode":"enforce","restrictions":{"t":{"allowedCallers":["c"]}}})";
    const fs::path p = dir.write("policy.json", doc);

    std::string err;
    auto resolved = resolveAccessPolicyArg(p.string(), &err);
    ASSERT_TRUE(resolved.has_value()) << err;
    EXPECT_EQ(nlohmann::json::parse(*resolved).value("mode", std::string{}), "enforce");
}

// ── Failures are loud ────────────────────────────────────────────────────────

TEST(AccessPolicyArg, MissingFileFailsWithAReason) {
    std::string err;
    auto resolved = resolveAccessPolicyArg("/definitely/not/here/policy.json", &err);
    EXPECT_FALSE(resolved.has_value());
    EXPECT_NE(err.find("could not be opened"), std::string::npos) << err;
}

TEST(AccessPolicyArg, MalformedInlineJsonFailsWithAReason) {
    std::string err;
    auto resolved = resolveAccessPolicyArg("{not valid json", &err);
    EXPECT_FALSE(resolved.has_value());
    EXPECT_NE(err.find("not valid JSON"), std::string::npos) << err;
}

TEST(AccessPolicyArg, MalformedFileJsonFailsWithAReason) {
    TempDir dir;
    const fs::path p = dir.write("bad.json", "{oops");
    std::string err;
    auto resolved = resolveAccessPolicyArg(p.string(), &err);
    EXPECT_FALSE(resolved.has_value());
    EXPECT_NE(err.find("not valid JSON"), std::string::npos) << err;
}

TEST(AccessPolicyArg, NullErrorPointerIsAccepted) {
    EXPECT_FALSE(resolveAccessPolicyArg("{nope", nullptr).has_value());
}
