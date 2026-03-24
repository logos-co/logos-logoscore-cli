#include "config.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <cstdlib>

QString Config::configDir()
{
    return QDir::homePath() + "/.logoscore";
}

QString Config::configFilePath()
{
    return configDir() + "/config.json";
}

QString Config::connectionFilePath()
{
    return configDir() + "/daemon.json";
}

QString Config::tokenFromEnv()
{
    const char* token = std::getenv("LOGOSCORE_TOKEN");
    return token ? QString::fromUtf8(token) : QString();
}

QString Config::tokenFromConfigFile()
{
    QFile file(configFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (doc.isObject())
        return doc.object().value("token").toString();
    return {};
}

QString Config::tokenFromConnectionFile()
{
    QFile file(connectionFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (doc.isObject())
        return doc.object().value("token").toString();
    return {};
}

QString Config::getToken()
{
    // Priority 1: environment variable
    QString token = tokenFromEnv();
    if (!token.isEmpty())
        return token;

    // Priority 2: config file
    token = tokenFromConfigFile();
    if (!token.isEmpty())
        return token;

    // Priority 3: connection file
    return tokenFromConnectionFile();
}

QJsonObject Config::load()
{
    QFile file(configFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    return doc.isObject() ? doc.object() : QJsonObject();
}
