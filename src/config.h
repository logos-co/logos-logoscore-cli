#ifndef CONFIG_H
#define CONFIG_H

#include <QString>
#include <QJsonObject>

class Config {
public:
    static QString getToken();
    static QString configDir();

    // Daemon-owned tree: <configDir>/daemon/{daemon.json, tokens/<name>.json}.
    // The daemon never reads anything outside daemon/.
    static QString daemonDir();
    static QString daemonConfigPath();   // <configDir>/daemon/daemon.json
    static QString daemonTokensDir();    // <configDir>/daemon/tokens

    // Client-owned tree: <configDir>/client/{client.json, <token_file>}.
    // The client never reads anything outside client/.
    static QString clientDir();
    static QString clientConfigPath();   // <configDir>/client/client.json
    // Path to the raw-token file inside client/, given its filename
    // (e.g. "auto.json"). Caller is expected to read the filename from
    // client.json's `token_file` field.
    static QString clientTokenPath(const QString& filename);

    // Override the config dir for the lifetime of the process. Called from main
    // when --config-dir is passed, so daemon + client agree on a single config
    // tree and parallel logoscore instances can coexist with distinct trees.
    // Pass an empty string to clear the override (tests).
    static void setConfigDir(const QString& path);

private:
    static QString tokenFromEnv();
};

#endif // CONFIG_H
