#ifndef OUTPUT_H
#define OUTPUT_H

#include <logos_json.h>
#include <string>

class Output {
public:
    Output(bool forceJson = false);

    bool isTTY() const;
    bool isJsonMode() const;
    void setJsonMode(bool json);
    // Force human-readable output even when stdout is not a TTY (e.g. piped).
    // Takes precedence over JSON auto-detection; the inverse of --json.
    void setHumanMode(bool human);

    // Success output — accepts any nlohmann::json value (object, array, etc.)
    void printSuccess(const nlohmann::json& data);
    void printSuccess(const std::string& message);

    // Error output
    void printError(const std::string& code, const std::string& message,
                    const LogosMap& extra = {});

    // Table output (list-modules, stats)
    void printModuleList(const LogosList& modules);
    void printStats(const LogosList& stats);
    void printStatus(const LogosMap& status);
    void printModuleInfo(const LogosMap& info);

    // Event output (watch)
    void printEvent(const LogosMap& event);

    // Reload output
    void printReload(const LogosMap& result);

    // Generic key-value display
    void printKeyValue(const std::string& key, const std::string& value);

    // Raw output to stdout
    void printRaw(const std::string& text);

private:
    bool m_forceJson;
    bool m_forceHuman = false;
    bool m_ttyChecked = false;
    bool m_isTTY = false;

    void checkTTY();
    std::string formatUptime(int64_t seconds) const;
    std::string padRight(const std::string& str, int width) const;
};

#endif // OUTPUT_H
