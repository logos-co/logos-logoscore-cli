#include <gtest/gtest.h>
#include <logos_json.h>
#include <sstream>
#include <cstdio>
#include "client/output.h"

// Helper to capture stdout
class CaptureStdout {
public:
    CaptureStdout() {
        oldBuf = std::cout.rdbuf(buffer.rdbuf());
    }
    ~CaptureStdout() {
        std::cout.rdbuf(oldBuf);
    }
    std::string str() const { return buffer.str(); }

private:
    std::stringstream buffer;
    std::streambuf* oldBuf;
};

class CaptureStderr {
public:
    CaptureStderr() {
        oldBuf = std::cerr.rdbuf(buffer.rdbuf());
    }
    ~CaptureStderr() {
        std::cerr.rdbuf(oldBuf);
    }
    std::string str() const { return buffer.str(); }

private:
    std::stringstream buffer;
    std::streambuf* oldBuf;
};

class OutputTest : public ::testing::Test {
protected:
    // Force JSON mode for predictable output in tests
    Output jsonOutput{true};
    Output humanOutput{false};  // Note: in test environment, TTY detection varies
};

// ── JSON Mode Tests ──────────────────────────────────────────────────────────

TEST_F(OutputTest, IsJsonMode_TrueWhenForced)
{
    EXPECT_TRUE(jsonOutput.isJsonMode());
}

TEST_F(OutputTest, PrintSuccess_JsonObject)
{
    CaptureStdout cap;
    LogosMap data{{"status", "ok"}, {"module", "waku"}};
    jsonOutput.printSuccess(data);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
    EXPECT_EQ(doc["module"].get<std::string>(), "waku");
}

TEST_F(OutputTest, PrintSuccess_JsonArray)
{
    CaptureStdout cap;
    LogosList data = nlohmann::json::array({
        LogosMap{{"name", "waku"}},
        LogosMap{{"name", "chat"}}
    });
    jsonOutput.printSuccess(data);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 2u);
}

TEST_F(OutputTest, PrintSuccess_String)
{
    CaptureStdout cap;
    jsonOutput.printSuccess(std::string("All good"));

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["status"].get<std::string>(), "ok");
    EXPECT_EQ(doc["message"].get<std::string>(), "All good");
}

TEST_F(OutputTest, PrintError_Json)
{
    CaptureStdout cap;
    jsonOutput.printError("MODULE_NOT_FOUND", "Module 'foo' not found.",
                          LogosMap{{"known_modules", LogosList::array({"waku", "chat"})}});

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["status"].get<std::string>(), "error");
    EXPECT_EQ(doc["code"].get<std::string>(), "MODULE_NOT_FOUND");
    EXPECT_EQ(doc["message"].get<std::string>(), "Module 'foo' not found.");
}

TEST_F(OutputTest, PrintModuleList_Json)
{
    CaptureStdout cap;
    LogosList modules = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"version", "0.1.0"}, {"status", "loaded"}, {"uptime_seconds", 8040}},
        LogosMap{{"name", "chat"}, {"version", "0.2.0"}, {"status", "crashed"}}
    });
    jsonOutput.printModuleList(modules);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 2u);
    EXPECT_EQ(doc[0]["name"].get<std::string>(), "waku");
    EXPECT_EQ(doc[1]["status"].get<std::string>(), "crashed");
}

TEST_F(OutputTest, PrintStatus_Json_Running)
{
    CaptureStdout cap;
    LogosMap status{
        {"daemon", LogosMap{
            {"status", "running"}, {"pid", 12345}, {"uptime_seconds", 3600}, {"version", "1.0"}
        }},
        {"modules_summary", LogosMap{{"loaded", 2}, {"crashed", 0}, {"not_loaded", 1}}},
        {"modules", nlohmann::json::array({
            LogosMap{{"name", "waku"}, {"status", "loaded"}},
            LogosMap{{"name", "chat"}, {"status", "loaded"}},
            LogosMap{{"name", "delivery"}, {"status", "not_loaded"}}
        })}
    };
    jsonOutput.printStatus(status);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["daemon"]["status"].get<std::string>(), "running");
}

TEST_F(OutputTest, PrintStatus_Json_NotRunning)
{
    CaptureStdout cap;
    LogosMap status{{"daemon", LogosMap{{"status", "not_running"}}}};
    jsonOutput.printStatus(status);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["daemon"]["status"].get<std::string>(), "not_running");
}

TEST_F(OutputTest, PrintModuleInfo_Json)
{
    CaptureStdout cap;
    LogosMap info{
        {"name", "chat"},
        {"version", "0.2.0"},
        {"status", "loaded"},
        {"pid", 23457},
        {"uptime_seconds", 8040},
        {"dependencies", nlohmann::json::array({"waku", "store"})},
        {"methods", nlohmann::json::array({
            LogosMap{
                {"name", "send_message"},
                {"parameters", nlohmann::json::array({LogosMap{{"name", "text"}, {"type", "QString"}}})},
                {"returnType", "QString"}
            }
        })}
    };
    jsonOutput.printModuleInfo(info);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["name"].get<std::string>(), "chat");
}

// Human-mode: the methods loop must read the real getPluginMethods keys
// (returnType / parameters) and render a multi-line description with each
// line preserved.
TEST_F(OutputTest, PrintModuleInfo_Human_MethodsAndDescriptions)
{
    CaptureStdout cap;
    LogosMap info{
        {"name", "chat"},
        {"version", "0.2.0"},
        {"status", "loaded"},
        {"methods", nlohmann::json::array({
            LogosMap{
                {"name", "send_message"},
                {"returnType", "QString"},
                {"parameters", nlohmann::json::array({LogosMap{{"name", "text"}, {"type", "QString"}}})},
                {"description", "Sends a message.\nUses the active channel."}
            }
        })}
    };
    humanOutput.printModuleInfo(info);

    std::string out = cap.str();
    // Signature is built from returnType + parameters (the keys this fixes).
    EXPECT_NE(out.find("send_message(text: QString) -> QString"), std::string::npos);
    // Multi-line description: both lines rendered, line breaks preserved.
    EXPECT_NE(out.find("Sends a message."), std::string::npos);
    EXPECT_NE(out.find("Uses the active channel."), std::string::npos);
}

// Human-mode: events render in their own section as `name(params)` with NO
// return type (events are fire-and-forget), plus their multi-line description.
TEST_F(OutputTest, PrintModuleInfo_Human_Events)
{
    CaptureStdout cap;
    LogosMap info{
        {"name", "chat"},
        {"version", "0.2.0"},
        {"status", "loaded"},
        {"events", nlohmann::json::array({
            LogosMap{
                {"name", "versionReady"},
                {"parameters", nlohmann::json::array({LogosMap{{"name", "version"}, {"type", "QString"}}})},
                {"description", "Fired when ready.\nCarries the version."}
            }
        })}
    };
    humanOutput.printModuleInfo(info);

    std::string out = cap.str();
    EXPECT_NE(out.find("Events:"), std::string::npos);
    // Signature has params but no `-> returnType` (events are void).
    EXPECT_NE(out.find("versionReady(version: QString)"), std::string::npos);
    EXPECT_EQ(out.find("versionReady(version: QString) ->"), std::string::npos);
    // Multi-line description: both lines rendered, line breaks preserved.
    EXPECT_NE(out.find("Fired when ready."), std::string::npos);
    EXPECT_NE(out.find("Carries the version."), std::string::npos);
}

TEST_F(OutputTest, PrintEvent_Ndjson)
{
    CaptureStdout cap;
    LogosMap event{
        {"timestamp", "2026-03-23T14:30:01Z"},
        {"module", "chat"},
        {"event", "chat-message"},
        {"data", LogosMap{{"from", "alice"}, {"text", "hello"}}}
    };
    jsonOutput.printEvent(event);

    std::string out = cap.str();
    EXPECT_NE(out.find("{"), std::string::npos);
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["event"].get<std::string>(), "chat-message");
}

TEST_F(OutputTest, PrintStats_Json)
{
    CaptureStdout cap;
    LogosList stats = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"pid", 23456}, {"cpu_percent", 2.1}, {"memory_mb", 48.3}},
        LogosMap{{"name", "chat"}, {"pid", 23457}, {"cpu_percent", 0.4}, {"memory_mb", 22.1}}
    });
    jsonOutput.printStats(stats);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_array());
    EXPECT_EQ(doc.size(), 2u);
}

// ── Human Mode Tests ─────────────────────────────────────────────────────────

TEST_F(OutputTest, PrintModuleList_Human)
{
    Output out(false);
    out.setJsonMode(false);

    CaptureStdout cap;
    LogosList modules = nlohmann::json::array({
        LogosMap{{"name", "waku"}, {"version", "0.1.0"}, {"status", "loaded"}, {"uptime_seconds", 8040}}
    });
    out.printModuleList(modules);
    std::string output = cap.str();
    EXPECT_FALSE(output.empty());
    // Version is rendered with a leading "v".
    EXPECT_NE(output.find("v0.1.0"), std::string::npos) << output;
}

// Regression for #59: a non-empty version must show in the human table, and a
// name longer than the old fixed 12-char column must not collide with it
// (the bug rendered "delivery_modulev" — name immediately followed by "v").
TEST_F(OutputTest, PrintModuleList_Human_LongNameDoesNotCollideWithVersion)
{
    Output out(false);
    out.setJsonMode(false);

    CaptureStdout cap;
    LogosList modules = nlohmann::json::array({
        LogosMap{{"name", "capability_module"}, {"version", "1.0.0"}, {"status", "loaded"}}
    });
    out.printModuleList(modules);
    std::string output = cap.str();

    EXPECT_NE(output.find("capability_module"), std::string::npos) << output;
    EXPECT_NE(output.find("v1.0.0"), std::string::npos) << output;
    // The name and version must be separated by whitespace, never fused.
    EXPECT_EQ(output.find("capability_modulev1.0.0"), std::string::npos)
        << "name collided with version column:\n" << output;
}

// A module that declares no version renders "-", not a bare "v".
TEST_F(OutputTest, PrintModuleList_Human_MissingVersionShowsDash)
{
    Output out(false);
    out.setJsonMode(false);

    CaptureStdout cap;
    LogosList modules = nlohmann::json::array({
        LogosMap{{"name", "no_version_module"}, {"status", "not_loaded"}}
    });
    out.printModuleList(modules);
    std::string output = cap.str();

    EXPECT_EQ(output.find("no_version_modulev"), std::string::npos)
        << "empty version must not render as a bare 'v':\n" << output;
    EXPECT_NE(output.find("-"), std::string::npos) << output;
}

// module-info human output shows the version with a "v" prefix when present.
TEST_F(OutputTest, PrintModuleInfo_Human_RendersVersion)
{
    Output out(false);
    out.setJsonMode(false);

    CaptureStdout cap;
    LogosMap info{{"name", "chat"}, {"version", "0.2.0"}, {"status", "loaded"}};
    out.printModuleInfo(info);
    std::string output = cap.str();
    EXPECT_NE(output.find("Version:       v0.2.0"), std::string::npos) << output;
}

// module-info with no version shows "-" rather than a bare "v".
TEST_F(OutputTest, PrintModuleInfo_Human_MissingVersionShowsDash)
{
    Output out(false);
    out.setJsonMode(false);

    CaptureStdout cap;
    LogosMap info{{"name", "chat"}, {"status", "not_loaded"}};
    out.printModuleInfo(info);
    std::string output = cap.str();
    EXPECT_NE(output.find("Version:       -"), std::string::npos) << output;
    EXPECT_EQ(output.find("Version:       v"), std::string::npos) << output;
}

TEST_F(OutputTest, PrintReload_Json_Success)
{
    CaptureStdout cap;
    LogosMap result{
        {"action", "reload"}, {"module", "chat"}, {"version", "0.2.0"},
        {"status", "loaded"}, {"pid", 51203}, {"previous_status", "crashed"}
    };
    jsonOutput.printReload(result);

    std::string out = cap.str();
    nlohmann::json doc = nlohmann::json::parse(out);
    ASSERT_TRUE(doc.is_object());
    EXPECT_EQ(doc["module"].get<std::string>(), "chat");
}

TEST_F(OutputTest, PrintError_Human)
{
    CaptureStderr cap;
    Output out(false);
    out.printError("NO_DAEMON", "No running logoscore daemon.");
    // Just verify it doesn't crash; actual output depends on TTY detection
}
