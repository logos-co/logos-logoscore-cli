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

    // Override the config dir for the lifetime of the process. Called from main
    // when --config-dir is passed, so daemon + client agree on a single
    // daemon.json location and parallel logoscore instances can coexist with
    // distinct config trees. Pass an empty string to clear the override (tests).
    static void setConfigDir(const QString& path);

private:
    static QString tokenFromEnv();
    static QString tokenFromConfigFile();
    static QString tokenFromConnectionFile();
};

#endif // CONFIG_H
