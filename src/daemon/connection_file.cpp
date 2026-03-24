#include "connection_file.h"
#include "../config.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>

#include <signal.h>

QString ConnectionFile::filePath()
{
    return Config::connectionFilePath();
}

bool ConnectionFile::isPidAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

bool ConnectionFile::write(const QString& instanceId, const QString& token,
                           qint64 pid, const QStringList& modulesDirs)
{
    QString dir = Config::configDir();
    if (!QDir().mkpath(dir))
        return false;

    QJsonObject obj;
    obj["instance_id"] = instanceId;
    obj["token"] = token;
    obj["pid"] = pid;
    obj["started_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonArray dirsArray;
    for (const QString& d : modulesDirs)
        dirsArray.append(d);
    obj["modules_dirs"] = dirsArray;

    QFile file(filePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

ConnectionInfo ConnectionFile::read()
{
    ConnectionInfo info;

    QFile file(filePath());
    if (!file.open(QIODevice::ReadOnly))
        return info;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject())
        return info;

    QJsonObject obj = doc.object();
    info.instanceId = obj.value("instance_id").toString();
    info.token = obj.value("token").toString();
    info.pid = static_cast<qint64>(obj.value("pid").toDouble(-1));
    info.startedAt = QDateTime::fromString(obj.value("started_at").toString(), Qt::ISODate);

    QJsonArray dirs = obj.value("modules_dirs").toArray();
    for (const QJsonValue& v : dirs)
        info.modulesDirs.append(v.toString());

    // Validate: check if PID is alive
    if (info.pid > 0 && isPidAlive(info.pid))
        info.valid = true;

    return info;
}

bool ConnectionFile::remove()
{
    return QFile::remove(filePath());
}

bool ConnectionFile::isStale()
{
    ConnectionInfo info = read();
    if (info.pid <= 0)
        return true;
    return !isPidAlive(info.pid);
}
