#include <gtest/gtest.h>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
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
    QJsonObject data{{"status", "ok"}, {"module", "waku"}};
    jsonOutput.printSuccess(data);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("status").toString(), "ok");
    EXPECT_EQ(doc.object().value("module").toString(), "waku");
}

TEST_F(OutputTest, PrintSuccess_JsonArray)
{
    CaptureStdout cap;
    QJsonArray data{QJsonObject{{"name", "waku"}}, QJsonObject{{"name", "chat"}}};
    jsonOutput.printSuccess(data);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isArray());
    EXPECT_EQ(doc.array().size(), 2);
}

TEST_F(OutputTest, PrintSuccess_String)
{
    CaptureStdout cap;
    jsonOutput.printSuccess(QString("All good"));

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("status").toString(), "ok");
    EXPECT_EQ(doc.object().value("message").toString(), "All good");
}

TEST_F(OutputTest, PrintError_Json)
{
    CaptureStdout cap;
    jsonOutput.printError("MODULE_NOT_FOUND", "Module 'foo' not found.",
                          QJsonObject{{"known_modules", QJsonArray{"waku", "chat"}}});

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("status").toString(), "error");
    EXPECT_EQ(doc.object().value("code").toString(), "MODULE_NOT_FOUND");
    EXPECT_EQ(doc.object().value("message").toString(), "Module 'foo' not found.");
}

TEST_F(OutputTest, PrintModuleList_Json)
{
    CaptureStdout cap;
    QJsonArray modules = {
        QJsonObject{{"name", "waku"}, {"version", "0.1.0"}, {"status", "loaded"}, {"uptime_seconds", 8040}},
        QJsonObject{{"name", "chat"}, {"version", "0.2.0"}, {"status", "crashed"}}
    };
    jsonOutput.printModuleList(modules);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isArray());
    EXPECT_EQ(doc.array().size(), 2);
    EXPECT_EQ(doc.array().at(0).toObject().value("name").toString(), "waku");
    EXPECT_EQ(doc.array().at(1).toObject().value("status").toString(), "crashed");
}

TEST_F(OutputTest, PrintStatus_Json_Running)
{
    CaptureStdout cap;
    QJsonObject status;
    status["daemon"] = QJsonObject{
        {"status", "running"}, {"pid", 12345}, {"uptime_seconds", 3600}, {"version", "1.0"}
    };
    status["modules_summary"] = QJsonObject{{"loaded", 2}, {"crashed", 0}, {"not_loaded", 1}};
    status["modules"] = QJsonArray{
        QJsonObject{{"name", "waku"}, {"status", "loaded"}},
        QJsonObject{{"name", "chat"}, {"status", "loaded"}},
        QJsonObject{{"name", "delivery"}, {"status", "not_loaded"}}
    };
    jsonOutput.printStatus(status);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("daemon").toObject().value("status").toString(), "running");
}

TEST_F(OutputTest, PrintStatus_Json_NotRunning)
{
    CaptureStdout cap;
    QJsonObject status;
    status["daemon"] = QJsonObject{{"status", "not_running"}};
    jsonOutput.printStatus(status);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("daemon").toObject().value("status").toString(), "not_running");
}

TEST_F(OutputTest, PrintModuleInfo_Json)
{
    CaptureStdout cap;
    QJsonObject info{
        {"name", "chat"},
        {"version", "0.2.0"},
        {"status", "loaded"},
        {"pid", 23457},
        {"uptime_seconds", 8040},
        {"dependencies", QJsonArray{"waku", "store"}},
        {"methods", QJsonArray{
            QJsonObject{
                {"name", "send_message"},
                {"params", QJsonArray{QJsonObject{{"name", "text"}, {"type", "QString"}}}},
                {"return_type", "QString"}
            }
        }}
    };
    jsonOutput.printModuleInfo(info);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("name").toString(), "chat");
}

TEST_F(OutputTest, PrintEvent_Ndjson)
{
    CaptureStdout cap;
    QJsonObject event{
        {"timestamp", "2026-03-23T14:30:01Z"},
        {"module", "chat"},
        {"event", "chat-message"},
        {"data", QJsonObject{{"from", "alice"}, {"text", "hello"}}}
    };
    jsonOutput.printEvent(event);

    std::string out = cap.str();
    // NDJSON: should be a single line of JSON
    EXPECT_NE(out.find("{"), std::string::npos);
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("event").toString(), "chat-message");
}

TEST_F(OutputTest, PrintStats_Json)
{
    CaptureStdout cap;
    QJsonArray stats = {
        QJsonObject{{"name", "waku"}, {"pid", 23456}, {"cpu_percent", 2.1}, {"memory_mb", 48.3}},
        QJsonObject{{"name", "chat"}, {"pid", 23457}, {"cpu_percent", 0.4}, {"memory_mb", 22.1}}
    };
    jsonOutput.printStats(stats);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isArray());
    EXPECT_EQ(doc.array().size(), 2);
}

// ── Human Mode Tests ─────────────────────────────────────────────────────────

TEST_F(OutputTest, PrintModuleList_Human)
{
    // Force human mode by setting JSON false and calling directly
    Output out(false);
    out.setJsonMode(false);

    // We can't easily test human output since it goes to stdout and
    // isTTY detection may not work in tests. But we verify the method doesn't crash.
    CaptureStdout cap;

    // Force the output object to think it's NOT json
    // In test environments stdout is often not a TTY so it defaults to JSON.
    // We test the JSON path above. Here we at least verify no crashes.
    QJsonArray modules = {
        QJsonObject{{"name", "waku"}, {"version", "0.1.0"}, {"status", "loaded"}, {"uptime_seconds", 8040}}
    };
    out.printModuleList(modules);
    std::string output = cap.str();
    EXPECT_FALSE(output.empty());
}

TEST_F(OutputTest, PrintReload_Json_Success)
{
    CaptureStdout cap;
    QJsonObject result{
        {"action", "reload"}, {"module", "chat"}, {"version", "0.2.0"},
        {"status", "loaded"}, {"pid", 51203}, {"previous_status", "crashed"}
    };
    jsonOutput.printReload(result);

    std::string out = cap.str();
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(out));
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object().value("module").toString(), "chat");
}

TEST_F(OutputTest, PrintError_Human)
{
    CaptureStderr cap;
    Output out(false);
    // In non-TTY environment, this may still go through JSON path,
    // but we ensure no crash
    out.printError("NO_DAEMON", "No running logoscore daemon.");
    // Just verify it doesn't crash; actual output depends on TTY detection
}
