#include "client_state.h"
#include "../config.h"
#include "../json_schema.h"
#include "../yaml_json.h"

#include <sstream>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string ClientStateFile::filePath()
{
    return Config::clientConfigPath();
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

// `path` is where this entry lives in the document ("daemon.core_service"), so
// a rejection can name it.
std::optional<ClientModuleTransport> transportFromJson(const json& j,
                                                       json_schema::Errors& errs,
                                                       const std::string& path)
{
    if (!j.is_object()) {
        errs.mismatch(path, "a mapping describing one transport", j);
        return std::nullopt;
    }
    json_schema::Reader r(j, errs, path + ".");
    ClientModuleTransport t;
    t.protocol = r.str("transport");
    // Strict allowlist — a typo in client/config.json's `transport`
    // would otherwise default downstream to LocalSocket and the
    // client would silently dial the wrong endpoint instead of
    // failing the parse with a clear schema error.
    if (t.protocol != "local"
     && t.protocol != "tcp"
     && t.protocol != "tcp_ssl") {
        errs.note(r.path("transport") +
                  R"(: expected one of "local", "tcp", "tcp_ssl")" +
                  (t.protocol.empty() ? std::string(", but it is missing.")
                                      : ", but got \"" + t.protocol + "\"."));
        return std::nullopt;
    }
    if (t.protocol != "local") {
        t.host = r.str("host");
        t.port = static_cast<uint16_t>(r.integer("port", 0, 0, 0xFFFF));
        t.codec = r.str("codec", "json");
    }
    if (t.protocol == "tcp_ssl") {
        t.caFile = r.str("ca");
        t.verifyPeer = r.boolean("verify_peer", true);
    }
    // A field of the wrong type is recorded in `errs`; the entry as a whole is
    // unusable, so hand back nothing rather than a half-read dial spec.
    if (!errs.ok()) return std::nullopt;
    return t;
}

} // namespace

std::optional<ClientState> parseClientStateDocument(const json& obj,
                                                    std::string* error)
{
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return std::optional<ClientState>{};
    };
    try {
        if (!obj.is_object())
            return fail("The config document must be a mapping at the top level.");

        json_schema::Errors errs;
        json_schema::Reader r(obj, errs);

        ClientState state;
        state.schemaVersion = static_cast<int>(r.integer("version", 0));
        if (!errs.ok()) return fail(errs.message());
        if (state.schemaVersion != kClientStateSchemaVersion)
            return fail("unsupported schema version " +
                        std::to_string(state.schemaVersion) +
                        " — relaunch the daemon to regenerate, or hand-edit "
                        "(expected version " +
                        std::to_string(kClientStateSchemaVersion) + ").");

        state.tokenFile  = r.str("token_file");
        state.instanceId = r.str("instance_id");

        if (const json* daemonObj = r.mapping("daemon")) {
            for (auto it = daemonObj->begin(); it != daemonObj->end(); ++it) {
                const std::string& moduleName = it.key();
                if (moduleName.empty()) continue;
                auto t = transportFromJson(it.value(), errs,
                                           r.path("daemon") + "." + moduleName);
                // Strict-parse contract: a typo in client/config.yaml
                // (e.g. transport=tcp_ssll) would otherwise silently
                // drop the entry, leaving the dial set incomplete and
                // surfacing as an obscure "no entry for core_service"
                // error later. Fail the whole parse so the caller
                // reports the broken config up front.
                if (!t) return fail(errs.ok() ? "the document could not be read."
                                              : errs.message());
                state.daemon.emplace(moduleName, *t);
            }
        }
        if (!errs.ok()) return fail(errs.message());

        // fileOk is about completeness, not validity: a document may be
        // perfectly well-formed and still not name a token file or a single
        // module to dial. That is a usable thing to install; it just isn't a
        // usable thing to connect with, which the caller reports separately.
        state.fileOk = !state.daemon.empty() && !state.tokenFile.empty();
        return state;
    } catch (const std::exception& e) {
        // Belt and braces — see parseDaemonConfigDocument.
        return fail(std::string("the document could not be read (") + e.what() + ").");
    }
}

ClientState ClientStateFile::read()
{
    if (auto& slot = overrideSlot(); slot.has_value())
        return *slot;

    std::ifstream ifs(filePath());
    if (!ifs) return ClientState{};

    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string parseError;
    auto parsed = yaml_json::parse(buf.str(), &parseError);
    if (!parsed) {
        std::cerr << "ClientState: " << filePath() << " is not valid YAML: "
                  << parseError << std::endl;
        return ClientState{};
    }

    // Exactly the validation `client config set` runs before it writes, so a
    // document that installs is a document the client can dial with.
    std::string error;
    auto state = parseClientStateDocument(*parsed, &error);
    if (!state) {
        std::cerr << "ClientState: " << filePath() << ": " << error << std::endl;
        return ClientState{};
    }
    return *state;
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
    ofs << (Config::flavor() == Config::Flavor::Modern
                ? yaml_json::dump(obj)
                : obj.dump(4) + "\n");
    return ofs.good();
}

std::string ClientStateFile::readTokenFile(const std::string& filename)
{
    if (filename.empty()) return {};
    std::ifstream ifs(Config::clientTokenPath(filename));
    if (!ifs) return {};
    json obj;
    try { obj = json::parse(ifs); }
    catch (...) { return {}; }
    // A `token` of the wrong type reads as absent, i.e. "no usable token",
    // which callers already handle. `value()` would have thrown.
    json_schema::Errors ignored;
    return json_schema::Reader(obj, ignored).str("token");
}
