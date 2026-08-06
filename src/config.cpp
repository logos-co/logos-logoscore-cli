#include "config.h"
#include <cstdlib>
#include <filesystem>
#include <map>

namespace {

// Root under which a session directory is created when nothing overrides it.
//
// Windows has no $HOME. The per-user, NON-ROAMING application-data root is
// %LOCALAPPDATA%, and non-roaming is the correct choice for everything a
// session holds: pids, sockets, ports and logs are all machine-specific, so
// following a roaming profile onto another machine would carry state that is
// wrong there. %USERPROFILE% is the fallback for the (unusual) case of
// LOCALAPPDATA being unset — never $HOME, which on Windows is normally absent
// and, when present, is set by a POSIX-emulation shell that does not agree
// with where the rest of the system thinks the user's files are.
std::string homeBase()
{
#ifdef _WIN32
    for (const char* var : { "LOCALAPPDATA", "USERPROFILE" }) {
        const char* v = std::getenv(var);
        if (v && *v) return std::string(v);
    }
    const char* tmp = std::getenv("TEMP");
    return tmp && *tmp ? std::string(tmp) : std::string(".");
#else
    // Unset -> /tmp, exactly as before. An *empty* HOME still yields "" here,
    // also as before; changing that would move where an existing session lives.
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp");
#endif
}

// Process-wide override set by setConfigDir(). Empty = not overridden.
// This is a plain static (not thread-local) — set once at startup from main(),
// read many times thereafter, never mutated concurrently with reads.
std::string& configDirOverride()
{
    static std::string s;
    return s;
}
}

void Config::setConfigDir(const std::string& path)
{
    configDirOverride() = path;
}

namespace {
// Default Legacy: a caller that never sets a flavor gets today's behaviour.
Config::Flavor& flavorSlot()
{
    static Config::Flavor f = Config::Flavor::Legacy;
    return f;
}
}  // namespace

void Config::setFlavor(Config::Flavor f) { flavorSlot() = f; }
Config::Flavor Config::flavor()          { return flavorSlot(); }

static bool isModern() { return Config::flavor() == Config::Flavor::Modern; }

std::string Config::configDir()
{
    // Precedence: explicit setter (from --config-dir) → LOGOSCTL_CONFIG_DIR
    // env var → ~/.logosctl. Parallel logosctl instances pick distinct
    // config trees so their daemon/ and client/ subdirs don't clash.
    const std::string& override = configDirOverride();
    if (!override.empty())
        return override;

    // Each tool reads its own env var, so exporting one in a shell cannot
    // silently redirect the other.
    const char* envDir = std::getenv(isModern() ? "LOGOSCTL_CONFIG_DIR"
                                                : "LOGOSCORE_CONFIG_DIR");
    if (envDir && *envDir)
        return std::string(envDir);

    return homeBase() + (isModern() ? "/.logosctl" : "/.logoscore");
}

std::string Config::daemonDir()        { return configDir() + "/daemon"; }
std::string Config::daemonConfigPath() { return daemonDir() + (isModern() ? "/config.yaml" : "/config.json"); }
std::string Config::daemonStatePath()  { return daemonDir() + "/state.json"; }
std::string Config::daemonTokensPath() { return daemonDir() + "/tokens.json"; }
std::string Config::daemonTokensDir()  { return daemonDir() + "/tokens"; }

std::string Config::clientDir()        { return configDir() + "/client"; }
std::string Config::clientConfigPath() { return clientDir() + (isModern() ? "/config.yaml" : "/config.json"); }

namespace {

std::map<Config::SessionDir, std::string>& sessionDirOverrides()
{
    static std::map<Config::SessionDir, std::string> m;
    return m;
}

// Resolve an override to a final absolute path.
//   ~/x  -> $HOME/x        (natural to write in a config file)
//   x    -> <configDir>/x  (stays inside the session, so still portable)
//   /x   -> /x             (explicitly outside the session)
std::string resolveOverride(const std::string& raw)
{
    if (raw.empty()) return raw;
    if (raw[0] == '~' && (raw.size() == 1 || raw[1] == '/')) {
        const std::string home = homeBase();
        if (!home.empty()) return home + raw.substr(1);
    }
    // "/x" is absolute everywhere this runs. On Windows it additionally has to
    // accept "C:\x" and "\\server\share", which std::filesystem answers and a
    // leading-slash test does not -- and getting that wrong would silently
    // reparent an operator's explicit absolute path under the session dir.
    // (fs::path("/x").is_absolute() is FALSE on Windows -- it is drive-relative
    // -- so the leading-slash test has to stay as well.)
    if (raw[0] == '/' || std::filesystem::path(raw).is_absolute()) return raw;
    return Config::configDir() + "/" + raw;
}

std::string sessionDir(Config::SessionDir which, const char* defaultName)
{
    auto& m = sessionDirOverrides();
    auto it = m.find(which);
    if (it != m.end() && !it->second.empty()) return it->second;
    return Config::configDir() + "/" + defaultName;
}

}  // namespace

void Config::setSessionDirOverride(SessionDir which, const std::string& path)
{
    if (path.empty()) sessionDirOverrides().erase(which);
    else              sessionDirOverrides()[which] = resolveOverride(path);
}

std::string Config::modulesDir() { return sessionDir(SessionDir::Modules, "modules"); }
std::string Config::pluginsDir() { return sessionDir(SessionDir::Plugins, "plugins"); }
std::string Config::keyringDir() { return sessionDir(SessionDir::Keyring, "keyring"); }
std::string Config::dataDir()    { return sessionDir(SessionDir::Data,    "data"); }
std::string Config::cacheDir()   { return sessionDir(SessionDir::Cache,   "cache"); }
std::string Config::logsDir()    { return sessionDir(SessionDir::Logs,    "logs"); }

std::string Config::downloadsDir()
{
    return (std::filesystem::path(cacheDir()) / "downloads").string();
}

std::string Config::clientTokenPath(const std::string& filename)
{
    // token_file is operator-influenced (--token-file / env / config.json); a
    // separator or ".." would escape client/ to read an arbitrary file as a
    // credential. Reject non-plain names and resolve to an in-client/ sentinel
    // that won't exist, so callers fail closed with "token file not found".
    const bool unsafe = filename.empty()
                     || filename.find('/')  != std::string::npos
                     || filename.find('\\') != std::string::npos
                     || filename.find("..") != std::string::npos;
    if (unsafe)
        return clientDir() + "/__invalid_token_filename__";
    return clientDir() + "/" + filename;
}

std::string Config::tokenFromEnv()
{
    const char* token = std::getenv(isModern() ? "LOGOSCTL_TOKEN" : "LOGOSCORE_TOKEN");
    return token ? std::string(token) : std::string();
}

std::string Config::getToken()
{
    // Priority 1: environment variable. Priority 2: the raw-token file
    // referenced by client/config.json's `token_file` field — that's
    // resolved inside ClientState since it requires parsing config.json.
    // This helper only handles the env override; ClientState falls back
    // to it when its own resolution misses.
    return tokenFromEnv();
}
