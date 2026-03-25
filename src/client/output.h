#ifndef OUTPUT_H
#define OUTPUT_H

#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>

class Output {
public:
    Output(bool forceJson = false);

    bool isTTY() const;
    bool isJsonMode() const;
    void setJsonMode(bool json);

    // Success output
    void printSuccess(const QJsonObject& data);
    void printSuccess(const QJsonArray& data);
    void printSuccess(const QString& message);

    // Error output
    void printError(const QString& code, const QString& message,
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
    void printKeyValue(const QString& key, const QString& value);

    // Raw output to stdout
    void printRaw(const QString& text);

private:
    bool m_forceJson;
    bool m_ttyChecked = false;
    bool m_isTTY = false;

    void checkTTY();
    QString formatUptime(qint64 seconds) const;
    QString padRight(const QString& str, int width) const;
};

#endif // OUTPUT_H
