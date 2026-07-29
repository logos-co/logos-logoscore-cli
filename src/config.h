#ifndef CONFIG_H
#define CONFIG_H

#include <string>

class Config {
public:
    // Which CLI is running. The two binaries ship side by side and must not
    // share a byte of state: `logosctl` is the new surface and is still being
    // validated, so a bad session of it must not be able to disturb a working
    // `logoscore` deployment. The flavor therefore selects the default config
    // directory, the env var consulted for an override, the config file names,
    // and the format they are written in.
    //
    // Legacy is the default so that anything which forgets to set it behaves
    // like the tool that exists today.
    enum class Flavor { Legacy, Modern };
    static void setFlavor(Flavor f);
    static Flavor flavor();

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
    // config.json for logoscore, config.yaml for logosctl.
    static std::string daemonConfigPath();
    static std::string daemonStatePath();    // <configDir>/daemon/state.json
    static std::string daemonTokensPath();   // <configDir>/daemon/tokens.json
    static std::string daemonTokensDir();    // <configDir>/daemon/tokens

    // Client-owned tree: <configDir>/client/{config.json, <token_file>}.
    // The client never reads anything outside client/.
    static std::string clientDir();
    static std::string clientConfigPath();
    // Path to the raw-token file inside client/, given its filename
    // (e.g. "auto.json"). Caller is expected to read the filename from
    // client/config.json's `token_file` field.
    static std::string clientTokenPath(const std::string& filename);

    // Session-owned tree. The config dir is the whole world for a session:
    // besides daemon/ and client/ state it holds the packages installed into
    // it, the trust material used to verify them, and the per-module
    // persistence. Copying the directory carries all of that with it, so a
    // session is portable and two sessions can hold different package sets
    // and different trust assumptions.
    //
    // These are the *writable* halves. Their read-only counterparts ship
    // beside the binary (paths::bundledModulesDir()); the package manager
    // scans both and lets the writable copy win on a name collision.
    static std::string modulesDir();   // <configDir>/modules
    static std::string pluginsDir();   // <configDir>/plugins
    static std::string keyringDir();   // <configDir>/keyring
    static std::string dataDir();      // <configDir>/data   (module persistence)
    static std::string cacheDir();     // <configDir>/cache  (downloaded .lgx)

    // Override the config dir for the lifetime of the process. Called from main
    // when --config-dir is passed, so daemon + client agree on a single config
    // tree and parallel logosctl instances can coexist with distinct trees.
    // Pass an empty string to clear the override (tests).
    static void setConfigDir(const std::string& path);

private:
    static std::string tokenFromEnv();
};

#endif // CONFIG_H
