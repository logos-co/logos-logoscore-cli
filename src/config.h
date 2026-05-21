#ifndef CONFIG_H
#define CONFIG_H

#include <string>

class Config {
public:
    static std::string getToken();
    static std::string configDir();

    // Daemon-owned tree: <configDir>/daemon/{config.json, state.json,
    // tokens.json, tokens/<name>.json}. The daemon never reads anything
    // outside daemon/. Three lifetimes by file:
    //   config.json  — operator preferences (written only on --persist-config)
    //   state.json   — live-instance resolved state (created at boot, deleted at shutdown)
    //   tokens.json  — hashed-at-rest token array (survives daemon restarts)
    //   tokens/      — raw, operator-copyable per-token files (0600)
    static std::string daemonDir();
    static std::string daemonConfigPath();   // <configDir>/daemon/config.json
    static std::string daemonStatePath();    // <configDir>/daemon/state.json
    static std::string daemonTokensPath();   // <configDir>/daemon/tokens.json
    static std::string daemonTokensDir();    // <configDir>/daemon/tokens

    // Client-owned tree: <configDir>/client/{config.json, <token_file>}.
    // The client never reads anything outside client/.
    static std::string clientDir();
    static std::string clientConfigPath();   // <configDir>/client/config.json
    // Path to the raw-token file inside client/, given its filename
    // (e.g. "auto.json"). Caller is expected to read the filename from
    // client/config.json's `token_file` field.
    static std::string clientTokenPath(const std::string& filename);

    // Override the config dir for the lifetime of the process. Called from main
    // when --config-dir is passed, so daemon + client agree on a single config
    // tree and parallel logoscore instances can coexist with distinct trees.
    // Pass an empty string to clear the override (tests).
    static void setConfigDir(const std::string& path);

private:
    static std::string tokenFromEnv();
};

#endif // CONFIG_H
