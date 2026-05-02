#ifndef CONFIG_H
#define CONFIG_H

#include <QString>
#include <QJsonObject>

class Config {
public:
    static QString getToken();
    static QString configDir();

    // Daemon-owned tree: <configDir>/daemon/{config.json, state.json,
    // tokens.json, tokens/<name>.json}. The daemon never reads anything
    // outside daemon/. Three lifetimes by file:
    //   config.json  — operator preferences (written only on --persist-config)
    //   state.json   — live-instance resolved state (created at boot, deleted at shutdown)
    //   tokens.json  — hashed-at-rest token array (survives daemon restarts)
    //   tokens/      — raw, operator-copyable per-token files (0600)
    static QString daemonDir();
    static QString daemonConfigPath();   // <configDir>/daemon/config.json
    static QString daemonStatePath();    // <configDir>/daemon/state.json
    static QString daemonTokensPath();   // <configDir>/daemon/tokens.json
    static QString daemonTokensDir();    // <configDir>/daemon/tokens

    // Client-owned tree: <configDir>/client/{config.json, <token_file>}.
    // The client never reads anything outside client/.
    static QString clientDir();
    static QString clientConfigPath();   // <configDir>/client/config.json
    // Path to the raw-token file inside client/, given its filename
    // (e.g. "auto.json"). Caller is expected to read the filename from
    // client/config.json's `token_file` field.
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
