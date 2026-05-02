#include "client_state.h"
#include "../config.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string ClientStateFile::filePath()
{
    return Config::clientConfigPath().toStdString();
}

namespace {
// Process-wide override set by setOverride(). Used by main.cpp to
// thread CLI-flag-merged client config into RpcClient::connect()
// without persisting to disk. Empty (nullopt) = no override → fall
// through to disk read in ClientStateFile::read().
std::optional<ClientState>& overrideSlot()
{
    static std::optional<ClientState> s;
    return s;
}
} // namespace

void ClientStateFile::setOverride(std::optional<ClientState> override)
{
    overrideSlot() = std::move(override);
}

namespace {

json transportToJson(const ClientModuleTransport& t)
{
    json j;
    j["transport"] = t.protocol;
    if (t.protocol != "local") {
        j["host"] = t.host;
        j["port"] = t.port;
        j["codec"] = t.codec.empty() ? std::string("json") : t.codec;
    }
    if (t.protocol == "tcp_ssl") {
        if (!t.caFile.empty()) j["ca"] = t.caFile;
        j["verify_peer"] = t.verifyPeer;
    }
    return j;
}

std::optional<ClientModuleTransport> transportFromJson(const json& j)
{
    if (!j.is_object()) return std::nullopt;
    ClientModuleTransport t;
    t.protocol = j.value("transport", std::string{});
    // Strict allowlist — a typo in client/config.json's `transport`
    // would otherwise default downstream to LocalSocket and the
    // client would silently dial the wrong endpoint instead of
    // failing the parse with a clear schema error.
    if (t.protocol != "local"
     && t.protocol != "tcp"
     && t.protocol != "tcp_ssl") return std::nullopt;
    if (t.protocol != "local") {
        t.host = j.value("host", std::string{});
        const int rawPort = j.value("port", 0);
        if (rawPort < 0 || rawPort > 0xFFFF) return std::nullopt;
        t.port = static_cast<uint16_t>(rawPort);
        t.codec = j.value("codec", std::string{"json"});
    }
    if (t.protocol == "tcp_ssl") {
        t.caFile = j.value("ca", std::string{});
        t.verifyPeer = j.value("verify_peer", true);
    }
    return t;
}

} // namespace

ClientState ClientStateFile::read()
{
    if (auto& slot = overrideSlot(); slot.has_value())
        return *slot;

    ClientState state;
    std::ifstream ifs(filePath());
    if (!ifs) return state;

    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return state; }

    state.schemaVersion = obj.value("version", 0);
    if (state.schemaVersion != kClientStateSchemaVersion) {
        std::cerr << "ClientState: unsupported schema version "
                  << state.schemaVersion << " in " << filePath()
                  << " — relaunch the daemon to regenerate, or hand-edit"
                     " (expected version " << kClientStateSchemaVersion
                  << ")." << std::endl;
        return state;
    }

    state.tokenFile  = obj.value("token_file", std::string{});
    state.instanceId = obj.value("instance_id", std::string{});

    if (obj.contains("daemon") && obj["daemon"].is_object()) {
        for (auto it = obj["daemon"].begin(); it != obj["daemon"].end(); ++it) {
            const std::string& moduleName = it.key();
            if (moduleName.empty()) continue;
            auto t = transportFromJson(it.value());
            if (!t) {
                // Strict-parse contract: a typo in client/config.json
                // (e.g. transport=tcp_ssll) would otherwise silently
                // drop the entry, leaving the dial set incomplete and
                // surfacing as an obscure "no entry for core_service"
                // error later. Fail the whole parse so the caller
                // sees fileOk=false and reports the broken config
                // up front.
                std::cerr << "ClientState: invalid transport entry under "
                          << "daemon." << moduleName << " in " << filePath()
                          << " — refusing to load partial config."
                          << std::endl;
                return ClientState{};
            }
            state.daemon.emplace(moduleName, *t);
        }
    }

    state.fileOk = !state.daemon.empty() && !state.tokenFile.empty();
    return state;
}

bool ClientStateFile::write(const ClientState& state)
{
    fs::path path(filePath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    json obj;
    obj["version"]    = kClientStateSchemaVersion;
    obj["token_file"] = state.tokenFile;
    if (!state.instanceId.empty())
        obj["instance_id"] = state.instanceId;

    json daemonObj = json::object();
    for (const auto& [name, t] : state.daemon)
        daemonObj[name] = transportToJson(t);
    obj["daemon"] = std::move(daemonObj);

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) return false;
    ofs << obj.dump(4) << "\n";
    return ofs.good();
}

std::string ClientStateFile::readTokenFile(const std::string& filename)
{
    if (filename.empty()) return {};
    std::ifstream ifs(Config::clientTokenPath(QString::fromStdString(filename)).toStdString());
    if (!ifs) return {};
    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return {}; }
    return obj.value("token", std::string{});
}
