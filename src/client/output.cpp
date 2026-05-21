#include "output.h"
#include "../string_utils.h"
#include <QJsonDocument>
#include <fmt/format.h>
#include <iostream>
#include <cstdio>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

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
    if (m_forceJson)
        return true;
    return !isTTY();
}

void Output::setJsonMode(bool json)
{
    m_forceJson = json;
}

std::string Output::formatUptime(qint64 seconds) const
{
    if (seconds < 0)
        return "-";
    if (seconds < 60)
        return fmt::format("{}s", seconds);
    if (seconds < 3600)
        return fmt::format("{}m", seconds / 60);

    qint64 hours = seconds / 3600;
    qint64 mins  = (seconds % 3600) / 60;
    return fmt::format("{}h {}m", hours, mins);
}

std::string Output::padRight(const std::string& str, int width) const
{
    return fmt::format("{:<{}}", str, width);
}

void Output::printSuccess(const QJsonObject& data)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(data).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    } else {
        for (auto it = data.begin(); it != data.end(); ++it) {
            std::string value;
            if (it.value().isString())
                value = it.value().toString().toStdString();
            else if (it.value().isDouble())
                value = fmt::format("{}", it.value().toDouble());
            else if (it.value().isBool())
                value = it.value().toBool() ? "true" : "false";
            else
                value = QJsonDocument(QJsonObject{{it.key(), it.value()}})
                            .toJson(QJsonDocument::Compact)
                            .toStdString();
            std::cout << it.key().toStdString() << ": " << value << std::endl;
        }
    }
}

void Output::printSuccess(const QJsonArray& data)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(data).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    }
}

void Output::printSuccess(const std::string& message)
{
    if (isJsonMode()) {
        QJsonObject obj;
        obj["status"]  = "ok";
        obj["message"] = QString::fromStdString(message);
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    } else {
        std::cout << message << std::endl;
    }
}

void Output::printError(const std::string& code, const std::string& message,
                        const QJsonObject& extra)
{
    if (isJsonMode()) {
        QJsonObject obj;
        obj["status"]  = "error";
        obj["code"]    = QString::fromStdString(code);
        obj["message"] = QString::fromStdString(message);
        for (auto it = extra.begin(); it != extra.end(); ++it)
            obj[it.key()] = it.value();
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    } else {
        std::cerr << "Error: " << message << std::endl;
    }
}

void Output::printModuleList(const QJsonArray& modules)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(modules).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    std::cout << padRight("NAME", 12)
              << padRight("VERSION", 10)
              << padRight("STATUS", 12)
              << "UPTIME" << std::endl;

    for (const QJsonValue& v : modules) {
        QJsonObject m = v.toObject();
        std::string name    = m.value("name").toString().toStdString();
        std::string version = "v" + m.value("version").toString().toStdString();
        std::string status  = m.value("status").toString().toStdString();
        std::string uptime  = "-";

        if (m.contains("uptime_seconds"))
            uptime = formatUptime(static_cast<qint64>(m.value("uptime_seconds").toDouble()));

        std::cout << padRight(name, 12)
                  << padRight(version, 10)
                  << padRight(status, 12)
                  << uptime << std::endl;
    }
}

void Output::printStats(const QJsonArray& stats)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(stats).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    std::cout << padRight("MODULE", 12)
              << padRight("PID", 8)
              << padRight("CPU%", 8)
              << "MEMORY" << std::endl;

    for (const QJsonValue& v : stats) {
        QJsonObject s = v.toObject();
        std::cout << padRight(s.value("name").toString().toStdString(), 12)
                  << padRight(fmt::format("{}", static_cast<qint64>(s.value("pid").toDouble())), 8)
                  << padRight(fmt::format("{:.1f}%", s.value("cpu_percent").toDouble()), 8)
                  << fmt::format("{:.1f} MB", s.value("memory_mb").toDouble())
                  << std::endl;
    }
}

void Output::printStatus(const QJsonObject& status)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(status).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    QJsonObject daemon = status.value("daemon").toObject();
    std::string daemonStatus = daemon.value("status").toString().toStdString();

    std::cout << "Logoscore Daemon" << std::endl;
    std::cout << "  Status:       " << daemonStatus << std::endl;

    if (daemonStatus == "not_running") {
        std::cout << std::endl;
        std::cout << "No daemon found." << std::endl;
        std::cout << "Run \"logoscore -D\" to start the daemon." << std::endl;
        return;
    }

    qint64 pid    = static_cast<qint64>(daemon.value("pid").toDouble());
    qint64 uptime = static_cast<qint64>(daemon.value("uptime_seconds").toDouble());
    std::string version    = daemon.value("version").toString().toStdString();
    std::string instanceId = daemon.value("instance_id").toString().toStdString();
    std::string socket     = daemon.value("socket").toString().toStdString();

    std::cout << "  PID:          " << pid << std::endl;
    std::cout << "  Uptime:       " << formatUptime(uptime) << std::endl;
    std::cout << "  Version:      v" << version << std::endl;

    if (!instanceId.empty())
        std::cout << "  Instance ID:  " << instanceId << std::endl;
    if (!socket.empty())
        std::cout << "  Socket:       " << socket << std::endl;

    QJsonObject summary = status.value("modules_summary").toObject();
    int loaded    = summary.value("loaded").toInt();
    int crashed   = summary.value("crashed").toInt();
    int notLoaded = summary.value("not_loaded").toInt();

    std::cout << std::endl;
    std::cout << "Modules: " << loaded << " loaded, "
              << crashed << " crashed, "
              << notLoaded << " not loaded" << std::endl;

    QJsonArray modules = status.value("modules").toArray();
    for (const QJsonValue& v : modules) {
        QJsonObject m = v.toObject();
        std::string name = m.value("name").toString().toStdString();
        std::string ver  = "v" + m.value("version").toString().toStdString();
        std::string st   = m.value("status").toString().toStdString();
        std::string up   = "-";
        if (m.contains("uptime_seconds"))
            up = formatUptime(static_cast<qint64>(m.value("uptime_seconds").toDouble()));

        std::cout << "  " << padRight(name, 12)
                  << padRight(ver, 10)
                  << padRight(st, 12)
                  << up << std::endl;
    }
}

void Output::printModuleInfo(const QJsonObject& info)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(info).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    std::string name    = info.value("name").toString().toStdString();
    std::string version = info.value("version").toString().toStdString();
    std::string status  = info.value("status").toString().toStdString();

    std::cout << "Name:          " << name    << std::endl;
    std::cout << "Version:       v" << version << std::endl;
    std::cout << "Status:        " << status  << std::endl;

    if (status == "loaded") {
        qint64 pid    = static_cast<qint64>(info.value("pid").toDouble());
        qint64 uptime = static_cast<qint64>(info.value("uptime_seconds").toDouble());

        std::cout << "PID:           " << pid << std::endl;
        std::cout << "Uptime:        " << formatUptime(uptime) << std::endl;
    } else if (status == "crashed") {
        if (info.contains("exit_code")) {
            int exitCode = info.value("exit_code").toInt();
            std::string signal  = info.value("crash_signal").toString().toStdString();
            std::string display = fmt::format("{}", exitCode);
            if (!signal.empty())
                display += " (" + signal + ")";
            std::cout << "Exit Code:     " << display << std::endl;
        }
        if (info.contains("crashed_at"))
            std::cout << "Crashed At:    "
                      << info.value("crashed_at").toString().toStdString() << std::endl;
        if (info.contains("restart_count"))
            std::cout << "Restart Count: " << info.value("restart_count").toInt() << std::endl;
        if (info.contains("last_log_line"))
            std::cout << "Last Log:      \""
                      << info.value("last_log_line").toString().toStdString()
                      << "\"" << std::endl;
    }

    // Dependencies
    QJsonArray deps = info.value("dependencies").toArray();
    if (!deps.isEmpty()) {
        std::vector<std::string> depList;
        for (const QJsonValue& v : deps)
            depList.push_back(v.toString().toStdString());
        std::cout << "Dependencies:  " << strutil::join(depList, ", ") << std::endl;
    }

    // Methods
    QJsonArray methods = info.value("methods").toArray();
    if (!methods.isEmpty()) {
        std::cout << std::endl;
        std::cout << "Methods:" << std::endl;
        for (const QJsonValue& v : methods) {
            QJsonObject method     = v.toObject();
            std::string methodName = method.value("name").toString().toStdString();
            std::string returnType = method.value("return_type").toString().toStdString();

            QJsonArray params = method.value("params").toArray();
            std::vector<std::string> paramStrs;
            for (const QJsonValue& p : params) {
                QJsonObject param = p.toObject();
                paramStrs.push_back(
                    param.value("name").toString().toStdString() + ": " +
                    param.value("type").toString().toStdString());
            }

            std::cout << "  " << methodName
                      << "(" << strutil::join(paramStrs, ", ") << ")"
                      << " -> " << returnType << std::endl;
        }
    }
}

void Output::printEvent(const QJsonObject& event)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(event).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        std::cout.flush();
        return;
    }

    std::string timestamp = event.value("timestamp").toString().toStdString();
    std::string module    = event.value("module").toString().toStdString();
    std::string eventName = event.value("event").toString().toStdString();
    QJsonObject data      = event.value("data").toObject();

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
        if (it.value().isString())
            value = it.value().toString().toStdString();
        else
            value = QJsonDocument(QJsonObject{{it.key(), it.value()}})
                        .toJson(QJsonDocument::Compact)
                        .toStdString();
        std::cout << "  " << it.key().toStdString() << ": " << value << std::endl;
    }
    std::cout << std::endl;
    std::cout.flush();
}

void Output::printReload(const QJsonObject& result)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    std::string status = result.value("status").toString().toStdString();
    std::string module = result.value("module").toString().toStdString();

    if (status == "loaded" || status == "ok") {
        std::string version = result.value("version").toString().toStdString();
        qint64 pid          = static_cast<qint64>(result.value("pid").toDouble());
        std::cout << "Module \"" << module
                  << "\" reloaded successfully (v" << version
                  << ", pid " << pid << ")" << std::endl;
    } else if (status == "error") {
        std::cerr << "Error: "
                  << result.value("error").toString().toStdString() << std::endl;
        if (result.contains("last_log_line"))
            std::cerr << "  Last log: \""
                      << result.value("last_log_line").toString().toStdString()
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
