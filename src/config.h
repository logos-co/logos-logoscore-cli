#ifndef CONFIG_H
#define CONFIG_H

#include <QString>
#include <QJsonObject>

class Config {
public:
    static QString getToken();
    static QJsonObject load();
    static QString configDir();
    static QString configFilePath();
    static QString connectionFilePath();

private:
    static QString tokenFromEnv();
    static QString tokenFromConfigFile();
    static QString tokenFromConnectionFile();
};

#endif // CONFIG_H
