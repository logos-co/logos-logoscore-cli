#include "config.h"
#include <QDir>
#include <cstdlib>

namespace {
// Process-wide override set by setConfigDir(). Empty = not overridden.
// This is a plain static (not thread-local) — set once at startup from main(),
// read many times thereafter, never mutated concurrently with reads.
QString& configDirOverride()
{
    static QString s;
    return s;
}
}

void Config::setConfigDir(const QString& path)
{
    configDirOverride() = path;
}

QString Config::configDir()
{
    // Precedence: explicit setter (from --config-dir) → LOGOSCORE_CONFIG_DIR
    // env var → ~/.logoscore. Parallel logoscore instances pick distinct
    // config trees so their daemon/ and client/ subdirs don't clash.
    const QString& override = configDirOverride();
    if (!override.isEmpty())
        return override;

    const char* envDir = std::getenv("LOGOSCORE_CONFIG_DIR");
    if (envDir && *envDir)
        return QString::fromUtf8(envDir);

    return QDir::homePath() + "/.logoscore";
}

QString Config::daemonDir()         { return configDir() + "/daemon"; }
QString Config::daemonConfigPath()  { return daemonDir() + "/config.json"; }
QString Config::daemonStatePath()   { return daemonDir() + "/state.json"; }
QString Config::daemonTokensPath()  { return daemonDir() + "/tokens.json"; }
QString Config::daemonTokensDir()   { return daemonDir() + "/tokens"; }

QString Config::clientDir()         { return configDir() + "/client"; }
QString Config::clientConfigPath()  { return clientDir() + "/config.json"; }

QString Config::clientTokenPath(const QString& filename)
{
    return clientDir() + "/" + filename;
}

QString Config::tokenFromEnv()
{
    const char* token = std::getenv("LOGOSCORE_TOKEN");
    return token ? QString::fromUtf8(token) : QString();
}

QString Config::getToken()
{
    // Priority 1: environment variable. Priority 2: the raw-token file
    // referenced by client/config.json's `token_file` field — that's
    // resolved inside ClientState since it requires parsing config.json.
    // This helper only handles the env override; ClientState falls back
    // to it when its own resolution misses.
    return tokenFromEnv();
}
