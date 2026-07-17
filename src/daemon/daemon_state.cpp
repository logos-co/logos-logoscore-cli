#include "daemon_state.h"
#include "../config.h"

#include <nlohmann/json.hpp>

#include <grp.h>        // getgrnam_r — resolve --access-group to a gid
#include <sys/stat.h>
#include <unistd.h>     // getpid — for unique temp-file names

#include <atomic>
#include <vector>

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
    if (!cfg.accessPolicy.empty()) obj["access_policy"] = cfg.accessPolicy;
    if (!cfg.accessGroup.empty())  obj["access_group"]  = cfg.accessGroup;
    return obj;
}

// Inverse of daemonConfigToJson — used by both config.json and
// state.json readers (the latter parses the `resolved` block).
//
// Returns std::nullopt when any embedded transport entry fails the
// strict-allowlist check in `transportFromJson` (e.g. an unknown
// `protocol` value, or an out-of-range port). Silent skip would
// turn a typo in config.json into a quietly-disabled listener — the
// daemon would come up with a partial transport set, no diagnostic.
// Failing the parse forces the operator to see the error and fix
// the file.
std::optional<DaemonConfig> daemonConfigFromJson(const json& obj)
{
    DaemonConfig cfg;
    cfg.modulesDirs     = obj.value("modules_dirs", std::vector<std::string>{});
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
                auto t = transportFromJson(j);
                if (!t) {
                    std::cerr << "DaemonConfig: invalid transport entry under "
                              << "modules." << moduleName
                              << " — refusing to load partial config."
                              << std::endl;
                    return std::nullopt;
                }
                transports.push_back(*t);
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
    cfg.accessPolicy = obj.value("access_policy", std::string{});
    cfg.accessGroup = obj.value("access_group", std::string{});
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
bool atomicWriteJson(const fs::path& path, const json& obj,
                     mode_t mode = S_IRUSR | S_IWUSR)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    // Per-writer-unique temp name (pid + counter) instead of a shared
    // "<path>.tmp": concurrent writers to the same destination would otherwise
    // truncate/rename the same temp and corrupt the result. The rename stays
    // the atomic publish step.
    static std::atomic<unsigned long> tmpSeq{0};
    const fs::path tmp = path.string() + ".tmp." +
        std::to_string(static_cast<long>(::getpid())) + "." +
        std::to_string(tmpSeq.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs) return false;
        ofs << obj.dump(4) << "\n";
        ofs.close();
        if (!ofs) return false;
    }
    // Apply the caller's mode to the temp file (default 0600). The rename
    // preserves it, so the destination lands at exactly `mode` regardless of
    // any pre-existing file's perms or the process umask.
    ::chmod(tmp.c_str(), mode);
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
    return Config::daemonConfigPath();
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
    return Config::daemonStatePath();
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
    if (obj.contains("resolved") && obj["resolved"].is_object()) {
        auto resolved = daemonConfigFromJson(obj["resolved"]);
        if (!resolved) {
            // Same fail-the-parse contract as DaemonConfigFile::read:
            // an invalid embedded transport entry means we can't trust
            // any of the resolved block. Return an empty (fileOk=false)
            // state so callers don't act on partial data.
            std::cerr << "DaemonRuntimeState: refusing to load partial "
                      << "resolved block from " << filePath() << std::endl;
            return DaemonRuntimeState{};
        }
        state.resolved = std::move(*resolved);
    }

    state.fileOk = !state.instanceId.empty();
    return state;
}

bool DaemonRuntimeStateFile::remove()
{
    std::error_code ec;
    return fs::remove(filePath(), ec);
}

namespace {

// Pick the transport a co-resident client should dial for a given
// module. Operator-typed order is the source of truth: prefer
// LocalSocket (always works on the same host) when present; otherwise
// fall through to whatever the operator named first. A TCP-only
// daemon emits a TCP client config, a TCP+local daemon emits local,
// and an operator-misordered TCP+local config still does the right
// thing because we explicitly look for a `local` entry first.
const TransportInfo* pickClientDialTransport(
    const std::vector<TransportInfo>& transports)
{
    if (transports.empty()) return nullptr;
    for (const auto& t : transports) {
        if (t.protocol == "local") return &t;
    }
    return &transports.front();
}

// Translate a server-side BIND address into a same-host DIAL address.
// Wildcard bind targets ("0.0.0.0", "::", "::0") aren't valid
// connect targets — a client that tries to connect to 0.0.0.0
// usually fails with "address not available" or hits whatever route
// the kernel happens to pick. Map them to loopback so the auto-
// emitted client/config.json (intended for a co-resident client)
// always has a working dial spec. daemon/state.json's advertised
// transport list is unaffected — that one keeps the operator's
// bind address verbatim because remote clients on a different host
// need it to reach the listener.
std::string toClientDialHost(const std::string& bindHost)
{
    if (bindHost.empty())            return "127.0.0.1";
    if (bindHost == "0.0.0.0")       return "127.0.0.1";
    if (bindHost == "::" ||
        bindHost == "::0")           return "::1";
    return bindHost;
}

// Serialize one TransportInfo into the per-module entry shape that
// client/config.json expects. The required fields depend on protocol;
// emit only what the dial side actually needs.
json toClientEntry(const TransportInfo& t)
{
    json entry;
    entry["transport"] = t.protocol;
    if (t.protocol == "tcp" || t.protocol == "tcp_ssl") {
        entry["host"] = toClientDialHost(t.host);
        entry["port"] = t.port;
        if (!t.codec.empty()) entry["codec"] = t.codec;
    }
    if (t.protocol == "tcp_ssl") {
        if (!t.caFile.empty()) entry["ca"] = t.caFile;
        // Auto-emitted local-client config is for same-host dialing
        // against the daemon we just bound. The daemon uses its own
        // cert/key; the client config doesn't need verifyPeer or CA
        // for the loopback case unless the operator explicitly set
        // them. Mirror what's in the resolved transport.
        entry["verify_peer"] = t.verifyPeer;
    }
    return entry;
}

// Resolve an OS group name-or-gid to a gid. Accepts an all-digits string as a
// numeric gid, otherwise looks the name up in the group database.
bool resolveGroupGid(const std::string& spec, gid_t& out)
{
    if (!spec.empty() &&
        spec.find_first_not_of("0123456789") == std::string::npos) {
        out = static_cast<gid_t>(std::strtoul(spec.c_str(), nullptr, 10));
        return true;
    }
    std::vector<char> buf(1024);
    struct group grp;
    struct group* result = nullptr;
    for (;;) {
        int rc = ::getgrnam_r(spec.c_str(), &grp, buf.data(), buf.size(), &result);
        if (rc == ERANGE && buf.size() < (1u << 20)) { buf.resize(buf.size() * 2); continue; }
        if (rc != 0 || result == nullptr) return false;
        out = grp.gr_gid;
        return true;
    }
}

}  // namespace

bool DaemonRuntimeStateFile::writeLocalClientArtifacts(
    const std::string& instanceId,
    const std::string& autoTokenRaw,
    const std::string& issuedAt,
    const std::vector<TransportInfo>& coreServiceTransports,
    const std::vector<TransportInfo>& capabilityModuleTransports,
    const std::string& accessGroup)
{
    const std::string clientDir      = Config::clientDir();
    const std::string clientCfgPath  = Config::clientConfigPath();
    const std::string autoTokenPath  = Config::clientTokenPath("auto.json");

    // Resolve the access group once. An unknown group name degrades to
    // owner-only (with a warning) rather than failing the whole boot — the
    // daemon still comes up, just not shared.
    gid_t groupGid = 0;
    bool  shareWithGroup = false;
    if (!accessGroup.empty()) {
        if (resolveGroupGid(accessGroup, groupGid)) {
            shareWithGroup = true;
        } else {
            std::cerr << "writeLocalClientArtifacts: unknown --access-group '"
                      << accessGroup << "' — client artifacts stay owner-only"
                      << std::endl;
        }
    }
    const mode_t fileMode = shareWithGroup ? static_cast<mode_t>(0640)
                                           : static_cast<mode_t>(S_IRUSR | S_IWUSR);

    std::error_code ec;
    fs::create_directories(clientDir, ec);
    if (ec) return false;

    if (shareWithGroup) {
        // Grant the group access to just the client subtree:
        //   - client/ becomes group r-x + owned by the group;
        //   - the config dir gets group traverse (execute) so a member can
        //     reach client/ without being able to list it;
        //   - daemon/ is locked to owner-only so tokens/state stay private
        //     even though the parent is now traversable.
        ::chown(clientDir.c_str(), static_cast<uid_t>(-1), groupGid);
        ::chmod(clientDir.c_str(), 0750);

        const std::string configDir = Config::configDir();
        struct stat cst;
        if (::stat(configDir.c_str(), &cst) == 0) {
            ::chown(configDir.c_str(), static_cast<uid_t>(-1), groupGid);
            ::chmod(configDir.c_str(), (cst.st_mode & 07777) | S_IXGRP);
        }
        const std::string daemonDir = Config::daemonDir();
        if (fs::exists(daemonDir, ec))
            ::chmod(daemonDir.c_str(), S_IRWXU);  // 0700
    }

    // client/config.json — dial config matching what the daemon actually
    // bound (mirrors the resolved transports so a co-resident client just
    // works; a hardcoded `local` used to hang against a TCP-only daemon).
    //
    // Decide whether to (re)write it:
    //   - Absent: always (re)generate. The instance_id changes every boot and
    //     this file is the client's only channel for it, so a persisted config
    //     dir that lost the file — or a second OS user who never had one — must
    //     get a current one back. (This is the pain a service operator hit:
    //     re-copying config.json by hand after every restart.)
    //   - Present with an instance_id that doesn't match this daemon: a stale
    //     copy of our own artifact (persisted dir, replaced daemon) — refresh
    //     it in place, preserving its token_file.
    //   - Present, matching (or operator-authored, no instance_id): left
    //     untouched so a hand-written remote config is never clobbered.
    bool writeClientCfg = false;
    std::string tokenFileName = "auto.json";
    if (!fs::exists(clientCfgPath, ec)) {
        writeClientCfg = true;
    } else {
        std::ifstream ifs(clientCfgPath);
        if (ifs) {
            json existing;
            try { existing = json::parse(ifs); }
            catch (...) { existing = json::object(); }
            const std::string existingInstance =
                existing.value("instance_id", std::string{});
            if (!existingInstance.empty() && existingInstance != instanceId) {
                writeClientCfg = true;
                // Keep whatever token file the existing config referenced —
                // an operator may have repointed it away from auto.json.
                tokenFileName = existing.value("token_file", std::string{"auto.json"});
            }
        }
    }

    if (writeClientCfg) {
        const TransportInfo* coreDial =
            pickClientDialTransport(coreServiceTransports);
        const TransportInfo* capDial =
            pickClientDialTransport(capabilityModuleTransports);

        json daemonBlock;
        daemonBlock["core_service"]      = coreDial ? toClientEntry(*coreDial)
                                                    : json({{"transport", "local"}});
        daemonBlock["capability_module"] = capDial  ? toClientEntry(*capDial)
                                                    : json({{"transport", "local"}});

        json client;
        client["version"]     = 2;
        client["token_file"]  = tokenFileName;
        client["instance_id"] = instanceId;
        client["daemon"]      = std::move(daemonBlock);

        // config.json carries no secret (the token lives in a separate file),
        // so it is safe to make group-readable when sharing.
        if (!atomicWriteJson(fs::path(clientCfgPath), client, fileMode)) return false;
        if (shareWithGroup)
            ::chown(clientCfgPath.c_str(), static_cast<uid_t>(-1), groupGid);
    }

    // client/auto.json — same shape as daemon/tokens/<name>.json.
    // Always (re)write: the daemon just (re)issued the auto token, so
    // any pre-existing client/auto.json now holds a stale value.
    json tokenFile;
    tokenFile["version"]   = 1;
    tokenFile["name"]      = "auto";
    tokenFile["token"]     = autoTokenRaw;
    tokenFile["issued_at"] = issuedAt;

    // The raw auto token is a credential. Owner-only by default; when sharing
    // with a group it is 0640 + chgrp so a group member can authenticate —
    // this is the whole point of --access-group (the docker.sock trust model:
    // group membership grants access).
    if (!atomicWriteJson(fs::path(autoTokenPath), tokenFile, fileMode)) return false;
    if (shareWithGroup)
        ::chown(autoTokenPath.c_str(), static_cast<uid_t>(-1), groupGid);
    return true;
}
