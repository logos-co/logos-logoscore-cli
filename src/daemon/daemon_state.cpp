#include "daemon_state.h"
#include "../config.h"

#include <nlohmann/json.hpp>

#include <sys/stat.h>

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string currentUtcIso8601()
{
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

namespace {

// Serialize one transport endpoint into the `transports` array under
// a module. Strips server-only secrets (cert/key) — only client-relevant
// fields make it to disk.
json transportToJson(const TransportInfo& t)
{
    json j;
    j["protocol"] = t.protocol;
    if (t.protocol != "local") {
        j["host"] = t.host;
        j["port"] = t.port;
        j["codec"] = t.codec.empty() ? std::string("json") : t.codec;
    }
    if (t.protocol == "tcp_ssl") {
        if (!t.caFile.empty()) j["ca_file"] = t.caFile;
        j["verify_peer"] = t.verifyPeer;
    }
    return j;
}

std::optional<TransportInfo> transportFromJson(const json& j)
{
    if (!j.is_object()) return std::nullopt;
    TransportInfo t;
    t.protocol = j.value("protocol", std::string{});
    // Strict allowlist. An unknown protocol ("local2", "tcps", etc.)
    // would otherwise default to LocalSocket downstream, silently
    // misconfiguring the daemon (a typo in config.json would mean no
    // TCP listener appears, with no visible diagnostic). Fail the
    // parse so callers see "schema-invalid" instead of "looked fine,
    // no listener bound".
    if (t.protocol != "local"
     && t.protocol != "tcp"
     && t.protocol != "tcp_ssl") return std::nullopt;
    t.host = j.value("host", std::string{});
    const int rawPort = j.value("port", 0);
    if (rawPort < 0 || rawPort > 0xFFFF) return std::nullopt;
    t.port = static_cast<uint16_t>(rawPort);
    t.caFile = j.value("ca_file", std::string{});
    t.verifyPeer = j.value("verify_peer", true);
    t.codec = j.value("codec", std::string{"json"});
    return t;
}

// Serialize the configuration block (preferences-or-resolved, same
// shape) into a JSON object. Used by both DaemonConfigFile (for
// config.json) and DaemonRuntimeStateFile (for state.json's
// `resolved` block).
json daemonConfigToJson(const DaemonConfig& cfg)
{
    json obj;
    obj["modules_dirs"]     = cfg.modulesDirs;
    obj["load_modules"]     = cfg.loadModules;
    obj["persistence_path"] = cfg.persistencePath;

    json modulesObj = json::object();
    for (const auto& [name, transports] : cfg.modules) {
        json arr = json::array();
        for (const auto& t : transports) arr.push_back(transportToJson(t));
        json moduleObj = json::object();
        moduleObj["transports"] = std::move(arr);
        modulesObj[name] = std::move(moduleObj);
    }
    obj["modules"] = std::move(modulesObj);

    json sslObj = json::object();
    sslObj["cert"] = cfg.sslCert;
    sslObj["key"]  = cfg.sslKey;
    sslObj["ca"]   = cfg.sslCa;
    obj["ssl"] = std::move(sslObj);

    obj["insecure_tcp"] = cfg.insecureTcp;
    return obj;
}

// Inverse of daemonConfigToJson — used by both config.json and
// state.json readers (the latter parses the `resolved` block).
DaemonConfig daemonConfigFromJson(const json& obj)
{
    DaemonConfig cfg;
    cfg.modulesDirs     = obj.value("modules_dirs", std::vector<std::string>{});
    cfg.loadModules     = obj.value("load_modules", "");
    cfg.persistencePath = obj.value("persistence_path", "");

    if (obj.contains("modules") && obj["modules"].is_object()) {
        for (auto it = obj["modules"].begin(); it != obj["modules"].end(); ++it) {
            const std::string& moduleName = it.key();
            if (moduleName.empty()) continue;
            const auto& moduleObj = it.value();
            if (!moduleObj.is_object()) continue;
            const auto& arr = moduleObj.value("transports", json::array());
            if (!arr.is_array()) continue;
            std::vector<TransportInfo> transports;
            for (const auto& j : arr) {
                if (auto t = transportFromJson(j)) transports.push_back(*t);
            }
            if (!transports.empty())
                cfg.modules.emplace(moduleName, std::move(transports));
        }
    }

    if (obj.contains("ssl") && obj["ssl"].is_object()) {
        cfg.sslCert = obj["ssl"].value("cert", std::string{});
        cfg.sslKey  = obj["ssl"].value("key",  std::string{});
        cfg.sslCa   = obj["ssl"].value("ca",   std::string{});
    }

    cfg.insecureTcp = obj.value("insecure_tcp", false);
    return cfg;
}

// Atomic *replace* helper used for both config.json and state.json.
// Writes to <path>.tmp, applies 0600, then renames into place. The
// rename is the atomic step — readers either see the pre-write file
// or the new one, never a half-written state. We do NOT fsync, so
// this isn't durable across power loss (close() flushes only
// userspace buffers; OS page cache is still in flight). Callers that
// need durability would need an explicit fsync(fd) on the temp file
// plus a dir fsync after rename. Returns false on any I/O step.
bool atomicWriteJson(const fs::path& path, const json& obj)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs) return false;
        ofs << obj.dump(4) << "\n";
        ofs.close();
        if (!ofs) return false;
    }
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
    std::error_code rec;
    fs::rename(tmp, path, rec);
    if (rec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

} // namespace

// -- DaemonConfigFile -------------------------------------------------------

std::string DaemonConfigFile::filePath()
{
    return Config::daemonConfigPath().toStdString();
}

std::optional<DaemonConfig> DaemonConfigFile::read()
{
    std::ifstream ifs(filePath());
    if (!ifs) return std::nullopt;

    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return std::nullopt; }

    const int v = obj.value("version", 0);
    if (v != kDaemonConfigSchemaVersion) {
        std::cerr << "DaemonConfig: unsupported schema version " << v
                  << " in " << filePath()
                  << " — relaunch with --persist-config to regenerate "
                  << "(expected version " << kDaemonConfigSchemaVersion
                  << ")." << std::endl;
        return std::nullopt;
    }
    return daemonConfigFromJson(obj);
}

bool DaemonConfigFile::write(const DaemonConfig& cfg)
{
    json obj = daemonConfigToJson(cfg);
    obj["version"] = kDaemonConfigSchemaVersion;
    return atomicWriteJson(fs::path(filePath()), obj);
}

// -- DaemonRuntimeStateFile -------------------------------------------------

std::string DaemonRuntimeStateFile::filePath()
{
    return Config::daemonStatePath().toStdString();
}

bool DaemonRuntimeStateFile::write(const DaemonRuntimeState& state)
{
    json obj;
    obj["version"]       = kDaemonRuntimeStateSchemaVersion;
    obj["instance_id"]   = state.instanceId;
    obj["pid"]           = state.pid;
    obj["started_at"]    = state.startedAt;
    if (!state.configSource.empty()) obj["config_source"] = state.configSource;
    obj["resolved"]      = daemonConfigToJson(state.resolved);
    return atomicWriteJson(fs::path(filePath()), obj);
}

DaemonRuntimeState DaemonRuntimeStateFile::read()
{
    DaemonRuntimeState state;

    std::ifstream ifs(filePath());
    if (!ifs) return state;

    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return state; }

    state.schemaVersion = obj.value("version", 0);
    if (state.schemaVersion != kDaemonRuntimeStateSchemaVersion) {
        std::cerr << "DaemonRuntimeState: unsupported schema version "
                  << state.schemaVersion
                  << " in " << filePath()
                  << " — relaunch the daemon to regenerate (expected version "
                  << kDaemonRuntimeStateSchemaVersion << ")." << std::endl;
        return state;
    }

    state.instanceId   = obj.value("instance_id", "");
    state.pid          = obj.value("pid", int64_t(-1));
    state.startedAt    = obj.value("started_at", "");
    state.configSource = obj.value("config_source", "");
    if (obj.contains("resolved") && obj["resolved"].is_object())
        state.resolved = daemonConfigFromJson(obj["resolved"]);

    state.fileOk = !state.instanceId.empty();
    return state;
}

bool DaemonRuntimeStateFile::remove()
{
    std::error_code ec;
    return fs::remove(filePath(), ec);
}

bool DaemonRuntimeStateFile::writeLocalClientArtifacts(
    const std::string& instanceId,
    const std::string& autoTokenRaw,
    const std::string& issuedAt,
    bool               freshTokensFile)
{
    const std::string clientDir      = Config::clientDir().toStdString();
    const std::string clientCfgPath  = Config::clientConfigPath().toStdString();
    const std::string autoTokenPath  = Config::clientTokenPath("auto.json").toStdString();

    std::error_code ec;
    fs::create_directories(clientDir, ec);
    if (ec) return false;

    // client/config.json — default LocalSocket dial config. Two
    // gates: (1) don't clobber an existing operator-written file;
    // (2) only emit during a "fresh" boot where daemon/tokens.json
    // didn't exist beforehand. The second gate keeps the daemon
    // out of the way once the operator has started managing tokens.
    if (freshTokensFile && !fs::exists(clientCfgPath, ec)) {
        json daemonBlock;
        daemonBlock["core_service"]      = { {"transport", "local"} };
        daemonBlock["capability_module"] = { {"transport", "local"} };

        json client;
        client["version"]     = 2;
        client["token_file"]  = "auto.json";
        client["instance_id"] = instanceId;
        client["daemon"]      = std::move(daemonBlock);

        if (!atomicWriteJson(fs::path(clientCfgPath), client)) return false;
    }

    // client/auto.json — same shape as daemon/tokens/<name>.json.
    // Always (re)write: the daemon just (re)issued the auto token, so
    // any pre-existing client/auto.json now holds a stale value.
    json tokenFile;
    tokenFile["version"]   = 1;
    tokenFile["name"]      = "auto";
    tokenFile["token"]     = autoTokenRaw;
    tokenFile["issued_at"] = issuedAt;

    if (!atomicWriteJson(fs::path(autoTokenPath), tokenFile)) return false;

    // Lock down the raw-token file (0600). atomicWriteJson already
    // applies it but be explicit at the destination too — the rename
    // preserves the temp file's mode, but a future code path that
    // skips atomicWriteJson should still land on 0600.
    ::chmod(autoTokenPath.c_str(), S_IRUSR | S_IWUSR);
    return true;
}
