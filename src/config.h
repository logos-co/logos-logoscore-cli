#ifndef CONFIG_H
#define CONFIG_H

#include <string>

class Config {
public:
    static std::string getToken();
    static std::string configDir();

    // Daemon-owned tree: <configDir>/daemon/{config.yaml, state.json,
    // tokens.json, tokens/<name>.json}. The daemon never reads anything
    // outside daemon/. Three lifetimes by file:
    //   config.yaml  — the operator's config (written by `daemon config set`)
    //   state.json   — live-instance resolved state (created at boot, deleted at shutdown)
    //   tokens.json  — hashed-at-rest token array (survives daemon restarts)
    //   tokens/      — raw, operator-copyable per-token files (0600)
    static std::string daemonDir();
    static std::string daemonConfigPath();   // <configDir>/daemon/config.yaml
    static std::string daemonStatePath();    // <configDir>/daemon/state.json
    static std::string daemonTokensPath();   // <configDir>/daemon/tokens.json
    static std::string daemonTokensDir();    // <configDir>/daemon/tokens

    // Client-owned tree: <configDir>/client/{config.yaml, <token_file>}.
    // The client never reads anything outside client/.
    static std::string clientDir();
    static std::string clientConfigPath();
    // Path to the raw-token file inside client/, given its filename
    // (e.g. "auto.json"). Caller is expected to read the filename from
    // client/config.yaml's `token_file` field.
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
    // Default to <configDir>/<name>, which is what makes a session portable.
    // Each can be redirected from the config's `dirs:` block -- to share one
    // keyring across sessions, to put the cache on a bigger disk, to point at
    // a read-only modules tree managed by something else.
    //
    // A relative override resolves inside the session, so it stays portable;
    // an absolute one deliberately opts out of that.
    static std::string modulesDir();   // <configDir>/modules
    static std::string pluginsDir();   // <configDir>/plugins
    static std::string keyringDir();   // <configDir>/keyring
    static std::string dataDir();      // <configDir>/data   (module persistence)
    static std::string cacheDir();     // <configDir>/cache  (downloaded .lgx)
    static std::string logsDir();      // <configDir>/logs   (daemon log files)

    // Where `package download` puts a .lgx when no -o was given. A subdirectory
    // of the cache rather than the cache root, so anything else that wants to
    // cache something later has somewhere to put it without colliding.
    static std::string downloadsDir(); // <cacheDir>/downloads

    // Redirect one of the above. An empty value clears the override. Applied
    // by the daemon once it has read the config; `~` and relative paths are
    // resolved here so every caller sees a final absolute path.
    enum class SessionDir { Modules, Plugins, Keyring, Data, Cache, Logs };
    static void setSessionDirOverride(SessionDir which, const std::string& path);

    // Override the config dir for the lifetime of the process. Called from main
    // when --config-dir is passed, so daemon + client agree on a single config
    // tree and parallel logosctl instances can coexist with distinct trees.
    // Pass an empty string to clear the override (tests).
    static void setConfigDir(const std::string& path);

private:
    static std::string tokenFromEnv();
};

#endif // CONFIG_H
