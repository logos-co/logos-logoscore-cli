#include "config.h"
#include <cstdlib>

namespace {
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

    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp")
         + (isModern() ? "/.logosctl" : "/.logoscore");
}

std::string Config::daemonDir()        { return configDir() + "/daemon"; }
std::string Config::daemonConfigPath() { return daemonDir() + (isModern() ? "/config.yaml" : "/config.json"); }
std::string Config::daemonStatePath()  { return daemonDir() + "/state.json"; }
std::string Config::daemonTokensPath() { return daemonDir() + "/tokens.json"; }
std::string Config::daemonTokensDir()  { return daemonDir() + "/tokens"; }

std::string Config::clientDir()        { return configDir() + "/client"; }
std::string Config::clientConfigPath() { return clientDir() + (isModern() ? "/config.yaml" : "/config.json"); }

std::string Config::modulesDir()       { return configDir() + "/modules"; }
std::string Config::pluginsDir()       { return configDir() + "/plugins"; }
std::string Config::keyringDir()       { return configDir() + "/keyring"; }
std::string Config::dataDir()          { return configDir() + "/data"; }
std::string Config::cacheDir()         { return configDir() + "/cache"; }

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
