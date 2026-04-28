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

std::string DaemonStateFile::filePath()
{
    return Config::daemonConfigPath().toStdString();
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
    if (t.protocol.empty()) return std::nullopt;
    t.host = j.value("host", std::string{});
    const int rawPort = j.value("port", 0);
    if (rawPort < 0 || rawPort > 0xFFFF) return std::nullopt;
    t.port = static_cast<uint16_t>(rawPort);
    t.caFile = j.value("ca_file", std::string{});
    t.verifyPeer = j.value("verify_peer", true);
    t.codec = j.value("codec", std::string{"json"});
    return t;
}

json tokenEntryToJson(const TokenEntry& e)
{
    json j;
    j["name"] = e.name;
    j["hash"] = e.hash;
    j["issued_at"] = e.issuedAt;
    // Persist `expires_at` as null when unset so the field is always
    // present and the on-disk shape is uniform across entries.
    if (e.expiresAt.empty()) j["expires_at"] = nullptr;
    else                     j["expires_at"] = e.expiresAt;
    j["local_only"] = e.localOnly;
    return j;
}

std::optional<TokenEntry> tokenEntryFromJson(const json& j)
{
    if (!j.is_object()) return std::nullopt;
    TokenEntry e;
    e.name     = j.value("name", std::string{});
    e.hash     = j.value("hash", std::string{});
    e.issuedAt = j.value("issued_at", std::string{});
    if (e.name.empty() || e.hash.empty()) return std::nullopt;
    if (j.contains("expires_at") && !j.at("expires_at").is_null())
        e.expiresAt = j.value("expires_at", std::string{});
    e.localOnly = j.value("local_only", false);
    return e;
}

} // namespace

bool DaemonStateFile::write(const DaemonState& state)
{
    fs::path path(filePath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    json obj;
    obj["version"]          = kDaemonStateSchemaVersion;
    obj["instance_id"]      = state.instanceId;
    obj["pid"]              = state.pid;
    obj["started_at"]       = state.startedAt;
    obj["modules_dirs"]     = state.modulesDirs;
    obj["load_modules"]     = state.loadModules;
    obj["persistence_path"] = state.persistencePath;

    json modulesObj = json::object();
    for (const auto& [name, transports] : state.modules) {
        json arr = json::array();
        for (const auto& t : transports)
            arr.push_back(transportToJson(t));
        json moduleObj = json::object();
        moduleObj["transports"] = std::move(arr);
        modulesObj[name] = std::move(moduleObj);
    }
    obj["modules"] = std::move(modulesObj);

    json sslObj = json::object();
    sslObj["cert"] = state.sslCert;
    sslObj["key"]  = state.sslKey;
    sslObj["ca"]   = state.sslCa;
    obj["ssl"] = std::move(sslObj);

    json tokensArr = json::array();
    for (const auto& t : state.tokens)
        tokensArr.push_back(tokenEntryToJson(t));
    obj["tokens"] = std::move(tokensArr);

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return false;
    ofs << obj.dump(4) << "\n";
    return ofs.good();
}

DaemonState DaemonStateFile::read()
{
    DaemonState state;

    std::ifstream ifs(filePath());
    if (!ifs) return state;

    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return state; }

    state.schemaVersion = obj.value("version", 0);
    if (state.schemaVersion != kDaemonStateSchemaVersion) {
        std::cerr << "DaemonState: unsupported schema version "
                  << state.schemaVersion
                  << " in " << filePath()
                  << " — relaunch the daemon to regenerate (expected version "
                  << kDaemonStateSchemaVersion << ")." << std::endl;
        return state;
    }

    state.instanceId      = obj.value("instance_id", "");
    state.pid             = obj.value("pid", int64_t(-1));
    state.startedAt       = obj.value("started_at", "");
    state.modulesDirs     = obj.value("modules_dirs", std::vector<std::string>{});
    state.loadModules     = obj.value("load_modules", "");
    state.persistencePath = obj.value("persistence_path", "");

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
                state.modules.emplace(moduleName, std::move(transports));
        }
    }

    if (obj.contains("ssl") && obj["ssl"].is_object()) {
        state.sslCert = obj["ssl"].value("cert", std::string{});
        state.sslKey  = obj["ssl"].value("key",  std::string{});
        state.sslCa   = obj["ssl"].value("ca",   std::string{});
    }

    if (obj.contains("tokens") && obj["tokens"].is_array()) {
        for (const auto& j : obj["tokens"]) {
            if (auto t = tokenEntryFromJson(j)) state.tokens.push_back(*t);
        }
    }

    state.fileOk = !state.instanceId.empty();
    return state;
}

bool DaemonStateFile::remove()
{
    std::error_code ec;
    return fs::remove(filePath(), ec);
}

bool DaemonStateFile::writeLocalClientArtifacts(const std::string& instanceId,
                                                const std::string& autoTokenRaw,
                                                const std::string& issuedAt)
{
    const std::string clientDir = Config::clientDir().toStdString();
    const std::string clientJsonPath = Config::clientConfigPath().toStdString();
    const std::string autoTokenPath  = Config::clientTokenPath("auto.json").toStdString();

    std::error_code ec;
    fs::create_directories(clientDir, ec);
    if (ec) return false;

    // client/client.json — minimal config that dials core_service +
    // capability_module over LocalSocket. We embed the daemon's
    // instance_id so the LocalSocket dial path (which derives the
    // socket name from `local:logos_<module>_<instance_id>`) doesn't
    // need to crack open daemon.json.
    {
        json daemonBlock;
        daemonBlock["core_service"]      = { {"transport", "local"} };
        daemonBlock["capability_module"] = { {"transport", "local"} };

        json client;
        client["version"]     = 1;
        client["token_file"]  = "auto.json";
        client["instance_id"] = instanceId;
        client["daemon"]      = std::move(daemonBlock);

        std::ofstream ofs(clientJsonPath, std::ios::trunc);
        if (!ofs) return false;
        ofs << client.dump(4) << "\n";
        if (!ofs.good()) return false;
    }

    // client/auto.json — the same shape as daemon/tokens/<name>.json,
    // so an operator running `cp daemon/tokens/auto.json
    // client/auto.json` (e.g. across hosts) round-trips cleanly.
    {
        json tokenFile;
        tokenFile["version"]   = 1;
        tokenFile["name"]      = "auto";
        tokenFile["token"]     = autoTokenRaw;
        tokenFile["issued_at"] = issuedAt;

        std::ofstream ofs(autoTokenPath, std::ios::trunc);
        if (!ofs) return false;
        ofs << tokenFile.dump(4) << "\n";
        if (!ofs.good()) return false;
    }

    // Lock down the raw-token file (0600). The whole `client/` dir
    // doesn't need 0700 because it's the operator's space — they may
    // share it with their own tooling.
    ::chmod(autoTokenPath.c_str(), S_IRUSR | S_IWUSR);
    return true;
}
