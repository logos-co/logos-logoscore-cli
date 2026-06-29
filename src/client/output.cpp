#include "output.h"
#include "../string_utils.h"
#include <fmt/format.h>
#include <algorithm>
#include <iostream>
#include <cstdio>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

// Human-readable module version: "v1.2.3" when present, "-" when the module
// declares none (modules aren't required to carry a version in metadata.json).
static std::string formatVersion(const std::string& version)
{
    return version.empty() ? "-" : "v" + version;
}

Output::Output(bool forceJson)
    : m_forceJson(forceJson)
{
}

void Output::checkTTY()
{
    if (!m_ttyChecked) {
        m_isTTY = ::isatty(::fileno(stdout)) != 0;
        m_ttyChecked = true;
    }
}

bool Output::isTTY() const
{
    const_cast<Output*>(this)->checkTTY();
    return m_isTTY;
}

bool Output::isJsonMode() const
{
    if (m_forceHuman)
        return false;
    if (m_forceJson)
        return true;
    return !isTTY();
}

void Output::setJsonMode(bool json)
{
    m_forceJson = json;
    if (json)
        m_forceHuman = false;
}

void Output::setHumanMode(bool human)
{
    m_forceHuman = human;
    if (human)
        m_forceJson = false;
}

std::string Output::formatUptime(int64_t seconds) const
{
    if (seconds < 0)
        return "-";
    if (seconds < 60)
        return fmt::format("{}s", seconds);
    if (seconds < 3600)
        return fmt::format("{}m", seconds / 60);

    int64_t hours = seconds / 3600;
    int64_t mins  = (seconds % 3600) / 60;
    return fmt::format("{}h {}m", hours, mins);
}

std::string Output::padRight(const std::string& str, int width) const
{
    return fmt::format("{:<{}}", str, width);
}

void Output::printSuccess(const nlohmann::json& data)
{
    if (isJsonMode()) {
        std::cout << data.dump() << std::endl;
    } else if (data.is_object()) {
        for (auto it = data.begin(); it != data.end(); ++it) {
            std::string value;
            const auto& val = it.value();
            if (val.is_string())       value = val.get<std::string>();
            else if (val.is_number())  value = fmt::format("{}", val.get<double>());
            else if (val.is_boolean()) value = val.get<bool>() ? "true" : "false";
            else                       value = val.dump();
            std::cout << it.key() << ": " << value << std::endl;
        }
    } else {
        std::cout << data.dump() << std::endl;
    }
}

void Output::printSuccess(const std::string& message)
{
    if (isJsonMode()) {
        LogosMap obj{{"status","ok"},{"message", message}};
        std::cout << obj.dump() << std::endl;
    } else {
        std::cout << message << std::endl;
    }
}

void Output::printError(const std::string& code, const std::string& message,
                        const LogosMap& extra)
{
    if (isJsonMode()) {
        LogosMap obj{{"status","error"},{"code", code},{"message", message}};
        for (auto it = extra.begin(); it != extra.end(); ++it)
            obj[it.key()] = it.value();
        std::cout << obj.dump() << std::endl;
    } else {
        std::cerr << "Error: " << message << std::endl;
    }
}

void Output::printModuleList(const LogosList& modules)
{
    if (isJsonMode()) {
        std::cout << modules.dump() << std::endl;
        return;
    }

    // Size the NAME/VERSION columns to their widest cell (real module names
    // run well past a fixed 12 chars, e.g. "capability_module"), so the
    // version never runs up against the name. +2 keeps a gap between columns.
    size_t nameWidth    = std::string("NAME").size();
    size_t versionWidth = std::string("VERSION").size();
    for (const auto& v : modules) {
        nameWidth    = std::max(nameWidth, v.value("name", std::string{}).size());
        versionWidth = std::max(versionWidth,
                                formatVersion(v.value("version", std::string{})).size());
    }
    nameWidth += 2;
    versionWidth += 2;

    std::cout << padRight("NAME", nameWidth)
              << padRight("VERSION", versionWidth)
              << padRight("STATUS", 12)
              << "UPTIME" << std::endl;

    for (const auto& v : modules) {
        std::string name    = v.value("name", std::string{});
        std::string version = formatVersion(v.value("version", std::string{}));
        std::string status  = v.value("status", std::string{});
        std::string uptime  = "-";

        if (v.contains("uptime_seconds"))
            uptime = formatUptime(static_cast<int64_t>(v["uptime_seconds"].get<double>()));

        std::cout << padRight(name, nameWidth)
                  << padRight(version, versionWidth)
                  << padRight(status, 12)
                  << uptime << std::endl;
    }
}

void Output::printStats(const LogosList& stats)
{
    if (isJsonMode()) {
        std::cout << stats.dump() << std::endl;
        return;
    }

    std::cout << padRight("MODULE", 12)
              << padRight("PID", 8)
              << padRight("CPU%", 8)
              << "MEMORY" << std::endl;

    for (const auto& s : stats) {
        std::cout << padRight(s.value("name", std::string{}), 12)
                  << padRight(fmt::format("{}", static_cast<int64_t>(s.value("pid", 0.0))), 8)
                  << padRight(fmt::format("{:.1f}%", s.value("cpu_percent", 0.0)), 8)
                  << fmt::format("{:.1f} MB", s.value("memory_mb", 0.0))
                  << std::endl;
    }
}

void Output::printStatus(const LogosMap& status)
{
    if (isJsonMode()) {
        std::cout << status.dump() << std::endl;
        return;
    }

    LogosMap daemon = status.value("daemon", LogosMap::object());
    std::string daemonStatus = daemon.value("status", std::string{});

    std::cout << "Logoscore Daemon" << std::endl;
    std::cout << "  Status:       " << daemonStatus << std::endl;

    if (daemonStatus == "not_running") {
        std::cout << std::endl;
        std::cout << "No daemon found." << std::endl;
        std::cout << "Run \"logoscore -D\" to start the daemon." << std::endl;
        return;
    }

    int64_t pid    = static_cast<int64_t>(daemon.value("pid", 0.0));
    int64_t uptime = static_cast<int64_t>(daemon.value("uptime_seconds", 0.0));
    std::string version    = daemon.value("version", std::string{});
    std::string instanceId = daemon.value("instance_id", std::string{});
    std::string socket     = daemon.value("socket", std::string{});

    std::cout << "  PID:          " << pid << std::endl;
    std::cout << "  Uptime:       " << formatUptime(uptime) << std::endl;
    std::cout << "  Version:      v" << version << std::endl;

    if (!instanceId.empty())
        std::cout << "  Instance ID:  " << instanceId << std::endl;
    if (!socket.empty())
        std::cout << "  Socket:       " << socket << std::endl;

    LogosMap summary = status.value("modules_summary", LogosMap::object());
    int loaded    = summary.value("loaded", 0);
    int crashed   = summary.value("crashed", 0);
    int notLoaded = summary.value("not_loaded", 0);

    std::cout << std::endl;
    std::cout << "Modules: " << loaded << " loaded, "
              << crashed << " crashed, "
              << notLoaded << " not loaded" << std::endl;

    LogosList modules = status.value("modules", LogosList::array());
    // Size NAME/VERSION columns to their content (see printModuleList).
    size_t nameWidth    = 0;
    size_t versionWidth = 0;
    for (const auto& m : modules) {
        nameWidth    = std::max(nameWidth, m.value("name", std::string{}).size());
        versionWidth = std::max(versionWidth,
                                formatVersion(m.value("version", std::string{})).size());
    }
    nameWidth    += 2;
    versionWidth += 2;
    for (const auto& m : modules) {
        std::string name = m.value("name", std::string{});
        std::string ver  = formatVersion(m.value("version", std::string{}));
        std::string st   = m.value("status", std::string{});
        std::string up   = "-";
        if (m.contains("uptime_seconds"))
            up = formatUptime(static_cast<int64_t>(m["uptime_seconds"].get<double>()));

        std::cout << "  " << padRight(name, nameWidth)
                  << padRight(ver, versionWidth)
                  << padRight(st, 12)
                  << up << std::endl;
    }
}

void Output::printModuleInfo(const LogosMap& info)
{
    if (isJsonMode()) {
        std::cout << info.dump() << std::endl;
        return;
    }

    std::string name    = info.value("name", std::string{});
    std::string version = info.value("version", std::string{});
    std::string status  = info.value("status", std::string{});

    std::cout << "Name:          " << name    << std::endl;
    std::cout << "Version:       " << formatVersion(version) << std::endl;
    std::cout << "Status:        " << status  << std::endl;

    if (status == "loaded") {
        int64_t pid    = static_cast<int64_t>(info.value("pid", 0.0));
        int64_t uptime = static_cast<int64_t>(info.value("uptime_seconds", 0.0));

        std::cout << "PID:           " << pid << std::endl;
        std::cout << "Uptime:        " << formatUptime(uptime) << std::endl;
    } else if (status == "crashed") {
        if (info.contains("exit_code")) {
            int exitCode = info.value("exit_code", 0);
            std::string signal  = info.value("crash_signal", std::string{});
            std::string display = fmt::format("{}", exitCode);
            if (!signal.empty())
                display += " (" + signal + ")";
            std::cout << "Exit Code:     " << display << std::endl;
        }
        if (info.contains("crashed_at"))
            std::cout << "Crashed At:    "
                      << info.value("crashed_at", std::string{}) << std::endl;
        if (info.contains("restart_count"))
            std::cout << "Restart Count: " << info.value("restart_count", 0) << std::endl;
        if (info.contains("last_log_line"))
            std::cout << "Last Log:      \""
                      << info.value("last_log_line", std::string{})
                      << "\"" << std::endl;
    }

    // Dependencies
    LogosList deps = info.value("dependencies", LogosList::array());
    if (!deps.empty()) {
        std::vector<std::string> depList;
        for (const auto& v : deps)
            depList.push_back(v.get<std::string>());
        std::cout << "Dependencies:  " << strutil::join(depList, ", ") << std::endl;
    }

    // Methods
    LogosList methods = info.value("methods", LogosList::array());
    if (!methods.empty()) {
        std::cout << std::endl;
        std::cout << "Methods:" << std::endl;
        for (const auto& v : methods) {
            std::string methodName = v.value("name", std::string{});
            std::string returnType = v.value("returnType", std::string{});
            std::string description = v.value("description", std::string{});

            LogosList params = v.value("parameters", LogosList::array());
            std::vector<std::string> paramStrs;
            for (const auto& p : params) {
                paramStrs.push_back(
                    p.value("name", std::string{}) + ": " +
                    p.value("type", std::string{}));
            }

            std::cout << "  " << methodName
                      << "(" << strutil::join(paramStrs, ", ") << ")"
                      << " -> " << returnType << std::endl;
            // Print each line of the (possibly multi-line) description indented,
            // preserving the doc comment's original line breaks.
            for (size_t start = 0; !description.empty() && start < description.size();) {
                size_t nl = description.find('\n', start);
                std::string dline = (nl == std::string::npos)
                    ? description.substr(start)
                    : description.substr(start, nl - start);
                std::cout << "      " << dline << std::endl;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    }

    // Events
    LogosList events = info.value("events", LogosList::array());
    if (!events.empty()) {
        std::cout << std::endl;
        std::cout << "Events:" << std::endl;
        for (const auto& v : events) {
            std::string eventName  = v.value("name", std::string{});
            std::string description = v.value("description", std::string{});

            LogosList params = v.value("parameters", LogosList::array());
            std::vector<std::string> paramStrs;
            for (const auto& p : params) {
                paramStrs.push_back(
                    p.value("name", std::string{}) + ": " +
                    p.value("type", std::string{}));
            }

            // Events are fire-and-forget — no return type.
            std::cout << "  " << eventName
                      << "(" << strutil::join(paramStrs, ", ") << ")" << std::endl;
            for (size_t start = 0; !description.empty() && start < description.size();) {
                size_t nl = description.find('\n', start);
                std::string dline = (nl == std::string::npos)
                    ? description.substr(start)
                    : description.substr(start, nl - start);
                std::cout << "      " << dline << std::endl;
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
        }
    }
}

void Output::printEvent(const LogosMap& event)
{
    if (isJsonMode()) {
        std::cout << event.dump() << std::endl;
        std::cout.flush();
        return;
    }

    std::string timestamp = event.value("timestamp", std::string{});
    std::string module    = event.value("module", std::string{});
    std::string eventName = event.value("event", std::string{});
    LogosMap data         = event.value("data", LogosMap::object());

    // Extract time portion from ISO timestamp
    auto tPos = timestamp.find('T');
    if (tPos != std::string::npos) {
        std::string timePart = timestamp.substr(tPos + 1);
        if (strutil::ends_with(timePart, 'Z'))
            timePart.pop_back();
        timestamp = timePart;
    }

    std::cout << "[" << timestamp << "] "
              << module << " :: " << eventName
              << std::endl;

    for (auto it = data.begin(); it != data.end(); ++it) {
        std::string value;
        const auto& val = it.value();
        if (val.is_string()) value = val.get<std::string>();
        else                 value = val.dump();
        std::cout << "  " << it.key() << ": " << value << std::endl;
    }
    std::cout << std::endl;
    std::cout.flush();
}

void Output::printReload(const LogosMap& result)
{
    if (isJsonMode()) {
        std::cout << result.dump() << std::endl;
        return;
    }

    std::string status = result.value("status", std::string{});
    std::string module = result.value("module", std::string{});

    if (status == "loaded" || status == "ok") {
        std::string version = formatVersion(result.value("version", std::string{}));
        int64_t pid         = static_cast<int64_t>(result.value("pid", 0.0));
        std::cout << "Module \"" << module
                  << "\" reloaded successfully (" << version
                  << ", pid " << pid << ")" << std::endl;
    } else if (status == "error") {
        std::cerr << "Error: "
                  << result.value("error", std::string{}) << std::endl;
        if (result.contains("last_log_line"))
            std::cerr << "  Last log: \""
                      << result.value("last_log_line", std::string{})
                      << "\"" << std::endl;
    }
}

void Output::printKeyValue(const std::string& key, const std::string& value)
{
    std::cout << key << ": " << value << std::endl;
}

void Output::printRaw(const std::string& text)
{
    std::cout << text << std::endl;
}
