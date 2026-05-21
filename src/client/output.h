#ifndef OUTPUT_H
#define OUTPUT_H

#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <string>

class Output {
public:
    Output(bool forceJson = false);

    bool isTTY() const;
    bool isJsonMode() const;
    void setJsonMode(bool json);

    // Success output
    void printSuccess(const QJsonObject& data);
    void printSuccess(const QJsonArray& data);
    void printSuccess(const std::string& message);

    // Error output
    void printError(const std::string& code, const std::string& message,
                    const QJsonObject& extra = {});

    // Table output (list-modules, stats)
    void printModuleList(const QJsonArray& modules);
    void printStats(const QJsonArray& stats);
    void printStatus(const QJsonObject& status);
    void printModuleInfo(const QJsonObject& info);

    // Event output (watch)
    void printEvent(const QJsonObject& event);

    // Reload output
    void printReload(const QJsonObject& result);

    // Generic key-value display
    void printKeyValue(const std::string& key, const std::string& value);

    // Raw output to stdout
    void printRaw(const std::string& text);

private:
    bool m_forceJson;
    bool m_ttyChecked = false;
    bool m_isTTY = false;

    void checkTTY();
    std::string formatUptime(qint64 seconds) const;
    std::string padRight(const std::string& str, int width) const;
};

#endif // OUTPUT_H
