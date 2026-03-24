#include "output.h"
#include <QJsonDocument>
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

QString Output::formatUptime(qint64 seconds) const
{
    if (seconds < 0)
        return "-";
    if (seconds < 60)
        return QString("%1s").arg(seconds);
    if (seconds < 3600)
        return QString("%1m").arg(seconds / 60);

    qint64 hours = seconds / 3600;
    qint64 mins = (seconds % 3600) / 60;
    return QString("%1h %2m").arg(hours).arg(mins);
}

QString Output::padRight(const QString& str, int width) const
{
    return str.leftJustified(width);
}

void Output::printSuccess(const QJsonObject& data)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(data).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    } else {
        // For generic JSON objects, print key-value pairs
        for (auto it = data.begin(); it != data.end(); ++it) {
            QString value;
            if (it.value().isString())
                value = it.value().toString();
            else if (it.value().isDouble())
                value = QString::number(it.value().toDouble());
            else if (it.value().isBool())
                value = it.value().toBool() ? "true" : "false";
            else
                value = QJsonDocument(QJsonObject{{it.key(), it.value()}}).toJson(QJsonDocument::Compact);
            std::cout << it.key().toStdString() << ": " << value.toStdString() << std::endl;
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

void Output::printSuccess(const QString& message)
{
    if (isJsonMode()) {
        QJsonObject obj;
        obj["status"] = "ok";
        obj["message"] = message;
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    } else {
        std::cout << message.toStdString() << std::endl;
    }
}

void Output::printError(const QString& code, const QString& message,
                        const QJsonObject& extra)
{
    if (isJsonMode()) {
        QJsonObject obj;
        obj["status"] = "error";
        obj["code"] = code;
        obj["message"] = message;
        for (auto it = extra.begin(); it != extra.end(); ++it)
            obj[it.key()] = it.value();
        std::cout << QJsonDocument(obj).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    } else {
        std::cerr << "Error: " << message.toStdString() << std::endl;
    }
}

void Output::printModuleList(const QJsonArray& modules)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(modules).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    // Human-readable table
    std::cout << padRight("NAME", 12).toStdString()
              << padRight("VERSION", 10).toStdString()
              << padRight("STATUS", 12).toStdString()
              << "UPTIME" << std::endl;

    for (const QJsonValue& v : modules) {
        QJsonObject m = v.toObject();
        QString name = m.value("name").toString();
        QString version = "v" + m.value("version").toString();
        QString status = m.value("status").toString();
        QString uptime = "-";

        if (m.contains("uptime_seconds"))
            uptime = formatUptime(static_cast<qint64>(m.value("uptime_seconds").toDouble()));

        std::cout << padRight(name, 12).toStdString()
                  << padRight(version, 10).toStdString()
                  << padRight(status, 12).toStdString()
                  << uptime.toStdString() << std::endl;
    }
}

void Output::printStats(const QJsonArray& stats)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(stats).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    std::cout << padRight("MODULE", 12).toStdString()
              << padRight("PID", 8).toStdString()
              << padRight("CPU%", 8).toStdString()
              << "MEMORY" << std::endl;

    for (const QJsonValue& v : stats) {
        QJsonObject s = v.toObject();
        std::cout << padRight(s.value("name").toString(), 12).toStdString()
                  << padRight(QString::number(static_cast<qint64>(s.value("pid").toDouble())), 8).toStdString()
                  << padRight(QString("%1%").arg(s.value("cpu_percent").toDouble(), 0, 'f', 1), 8).toStdString()
                  << QString("%1 MB").arg(s.value("memory_mb").toDouble(), 0, 'f', 1).toStdString()
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
    QString daemonStatus = daemon.value("status").toString();

    std::cout << "Logoscore Daemon" << std::endl;
    std::cout << "  Status:       " << daemonStatus.toStdString() << std::endl;

    if (daemonStatus == "not_running") {
        std::cout << std::endl;
        std::cout << "No daemon found." << std::endl;
        std::cout << "Run \"logoscore -D\" to start the daemon." << std::endl;
        return;
    }

    qint64 pid = static_cast<qint64>(daemon.value("pid").toDouble());
    qint64 uptime = static_cast<qint64>(daemon.value("uptime_seconds").toDouble());
    QString version = daemon.value("version").toString();
    QString instanceId = daemon.value("instance_id").toString();
    QString socket = daemon.value("socket").toString();

    std::cout << "  PID:          " << pid << std::endl;
    std::cout << "  Uptime:       " << formatUptime(uptime).toStdString() << std::endl;
    std::cout << "  Version:      v" << version.toStdString() << std::endl;

    if (!instanceId.isEmpty())
        std::cout << "  Instance ID:  " << instanceId.toStdString() << std::endl;
    if (!socket.isEmpty())
        std::cout << "  Socket:       " << socket.toStdString() << std::endl;

    QJsonObject summary = status.value("modules_summary").toObject();
    int loaded = summary.value("loaded").toInt();
    int crashed = summary.value("crashed").toInt();
    int notLoaded = summary.value("not_loaded").toInt();

    std::cout << std::endl;
    std::cout << "Modules: " << loaded << " loaded, "
              << crashed << " crashed, "
              << notLoaded << " not loaded" << std::endl;

    QJsonArray modules = status.value("modules").toArray();
    for (const QJsonValue& v : modules) {
        QJsonObject m = v.toObject();
        QString name = m.value("name").toString();
        QString ver = "v" + m.value("version").toString();
        QString st = m.value("status").toString();
        QString up = "-";
        if (m.contains("uptime_seconds"))
            up = formatUptime(static_cast<qint64>(m.value("uptime_seconds").toDouble()));

        std::cout << "  " << padRight(name, 12).toStdString()
                  << padRight(ver, 10).toStdString()
                  << padRight(st, 12).toStdString()
                  << up.toStdString() << std::endl;
    }
}

void Output::printModuleInfo(const QJsonObject& info)
{
    if (isJsonMode()) {
        std::cout << QJsonDocument(info).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        return;
    }

    QString name = info.value("name").toString();
    QString version = info.value("version").toString();
    QString status = info.value("status").toString();

    std::cout << "Name:          " << name.toStdString() << std::endl;
    std::cout << "Version:       v" << version.toStdString() << std::endl;
    std::cout << "Status:        " << status.toStdString() << std::endl;

    if (status == "loaded") {
        qint64 pid = static_cast<qint64>(info.value("pid").toDouble());
        qint64 uptime = static_cast<qint64>(info.value("uptime_seconds").toDouble());

        std::cout << "PID:           " << pid << std::endl;
        std::cout << "Uptime:        " << formatUptime(uptime).toStdString() << std::endl;
    } else if (status == "crashed") {
        if (info.contains("exit_code")) {
            int exitCode = info.value("exit_code").toInt();
            QString signal = info.value("crash_signal").toString();
            QString display = QString::number(exitCode);
            if (!signal.isEmpty())
                display += " (" + signal + ")";
            std::cout << "Exit Code:     " << display.toStdString() << std::endl;
        }
        if (info.contains("crashed_at"))
            std::cout << "Crashed At:    " << info.value("crashed_at").toString().toStdString() << std::endl;
        if (info.contains("restart_count"))
            std::cout << "Restart Count: " << info.value("restart_count").toInt() << std::endl;
        if (info.contains("last_log_line"))
            std::cout << "Last Log:      \"" << info.value("last_log_line").toString().toStdString() << "\"" << std::endl;
    }

    // Dependencies
    QJsonArray deps = info.value("dependencies").toArray();
    if (!deps.isEmpty()) {
        QStringList depList;
        for (const QJsonValue& v : deps)
            depList.append(v.toString());
        std::cout << "Dependencies:  " << depList.join(", ").toStdString() << std::endl;
    }

    // Methods
    QJsonArray methods = info.value("methods").toArray();
    if (!methods.isEmpty()) {
        std::cout << std::endl;
        std::cout << "Methods:" << std::endl;
        for (const QJsonValue& v : methods) {
            QJsonObject method = v.toObject();
            QString methodName = method.value("name").toString();
            QString returnType = method.value("return_type").toString();

            QJsonArray params = method.value("params").toArray();
            QStringList paramStrs;
            for (const QJsonValue& p : params) {
                QJsonObject param = p.toObject();
                paramStrs.append(param.value("name").toString() + ": " + param.value("type").toString());
            }

            std::cout << "  " << methodName.toStdString()
                      << "(" << paramStrs.join(", ").toStdString() << ")"
                      << " -> " << returnType.toStdString() << std::endl;
        }
    }
}

void Output::printEvent(const QJsonObject& event)
{
    if (isJsonMode()) {
        // NDJSON: one JSON object per line
        std::cout << QJsonDocument(event).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
        std::cout.flush();
        return;
    }

    QString timestamp = event.value("timestamp").toString();
    QString module = event.value("module").toString();
    QString eventName = event.value("event").toString();
    QJsonObject data = event.value("data").toObject();

    // Extract time portion from ISO timestamp
    if (timestamp.contains('T')) {
        QString timePart = timestamp.mid(timestamp.indexOf('T') + 1);
        if (timePart.endsWith('Z'))
            timePart.chop(1);
        timestamp = timePart;
    }

    std::cout << "[" << timestamp.toStdString() << "] "
              << module.toStdString() << " :: " << eventName.toStdString()
              << std::endl;

    for (auto it = data.begin(); it != data.end(); ++it) {
        QString value;
        if (it.value().isString())
            value = it.value().toString();
        else
            value = QJsonDocument(QJsonObject{{it.key(), it.value()}}).toJson(QJsonDocument::Compact);
        std::cout << "  " << it.key().toStdString() << ": " << value.toStdString() << std::endl;
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

    QString status = result.value("status").toString();
    QString module = result.value("module").toString();

    if (status == "loaded" || status == "ok") {
        QString version = result.value("version").toString();
        qint64 pid = static_cast<qint64>(result.value("pid").toDouble());
        std::cout << "Module \"" << module.toStdString()
                  << "\" reloaded successfully (v" << version.toStdString()
                  << ", pid " << pid << ")" << std::endl;
    } else if (status == "error") {
        std::cerr << "Error: " << result.value("error").toString().toStdString() << std::endl;
        if (result.contains("last_log_line"))
            std::cerr << "  Last log: \"" << result.value("last_log_line").toString().toStdString()
                      << "\"" << std::endl;
    }
}

void Output::printKeyValue(const QString& key, const QString& value)
{
    std::cout << key.toStdString() << ": " << value.toStdString() << std::endl;
}

void Output::printRaw(const QString& text)
{
    std::cout << text.toStdString() << std::endl;
}
