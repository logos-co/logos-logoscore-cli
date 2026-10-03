#include "daemon_state.h"
#include "../config.h"
#include "../json_schema.h"
#include "../removed_transports.h"
#include "../yaml_json.h"

#include <nlohmann/json.hpp>

#include "../platform_compat.h"

#ifndef _WIN32
#include <grp.h>        // getgrnam_r — resolve --access-group to a gid
#endif
#include <sys/stat.h>
#include <unistd.h>     // getpid — for unique temp-file names

#include <atomic>
#include <cerrno>
#include <limits>
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
    logosctl::gmtimeR(&tt, &utc);
    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

namespace {

// Serialize the configuration block (preferences-or-resolved, same
// shape) into a JSON object. Used by both DaemonConfigFile (for
// config.json) and DaemonRuntimeStateFile (for state.json's
// `resolved` block).
json daemonConfigToJson(const DaemonConfig& cfg)
{
    json obj;
    obj["modules_dirs"]     = cfg.modulesDirs;
    obj["persistence_path"] = cfg.persistencePath;

    // Only emit `dirs` when something is actually redirected, so a default
    // config stays free of noise the reader has to interpret.
    json dirsObj = json::object();
    if (!cfg.dirs.modules.empty()) dirsObj["modules"] = cfg.dirs.modules;
    if (!cfg.dirs.plugins.empty()) dirsObj["plugins"] = cfg.dirs.plugins;
    if (!cfg.dirs.keyring.empty()) dirsObj["keyring"] = cfg.dirs.keyring;
    if (!cfg.dirs.data.empty())    dirsObj["data"]    = cfg.dirs.data;
    if (!cfg.dirs.cache.empty())   dirsObj["cache"]   = cfg.dirs.cache;
    if (!cfg.dirs.logs.empty())    dirsObj["logs"]    = cfg.dirs.logs;
    if (!dirsObj.empty()) obj["dirs"] = std::move(dirsObj);

    json logObj = json::object();
    logObj["enabled"]     = cfg.logging.enabled;
    logObj["file"]        = cfg.logging.file;
    logObj["max_size_mb"] = cfg.logging.maxSizeMb;
    logObj["max_files"]   = cfg.logging.maxFiles;
    logObj["console"]     = cfg.logging.console;
    obj["logging"] = std::move(logObj);

    if (!cfg.accessPolicy.empty()) obj["access_policy"] = cfg.accessPolicy;
    if (!cfg.placement.empty())    obj["placement"]     = cfg.placement;
    if (!cfg.bundledModulesDirs.empty()) obj["bundled_modules_dirs"] = cfg.bundledModulesDirs;
    if (!cfg.accessGroup.empty())  obj["access_group"]  = cfg.accessGroup;
    // Omitted when unset, like access_policy: an empty string is not a valid
    // policy, so emitting one would produce a document that fails its own
    // reader on the next load.
    if (!cfg.signaturePolicy.empty()) obj["signature_policy"] = cfg.signaturePolicy;
    if (!cfg.peering.empty()) obj["peering"] = json::parse(cfg.peering, nullptr, false);
    return obj;
}

// `modules:`, `ssl:` and `insecure_tcp:` went with the tcp and tcp_ssl transports.
// What every written config carried still loads, and is dropped; the rest is refused.
bool removedTransportKeysLoad(const json& obj, const json_schema::Reader& r)
{
    json_schema::Errors& errs = r.errors();
    if (const json* modules = r.mapping("modules")) {
        for (auto it = modules->begin(); it != modules->end(); ++it) {
            const json& moduleObj = it.value();
            const std::string modulePath = r.path("modules") + "." + it.key();
            // An empty value (`core_service:` with nothing after it) names no transports.
            if (moduleObj.is_null()) continue;
            // `<module>: { transports: [ ... ] }` as written, or a bare list.
            const json* arr = nullptr;
            if (moduleObj.is_array()) {
                arr = &moduleObj;
            } else if (moduleObj.is_object()) {
                json_schema::Reader mr(moduleObj, errs, modulePath + ".");
                arr = mr.list("transports", "a list of transports");
                if (!arr) {
                    if (!errs.ok()) return false;
                    continue;  // no `transports` key: no entries, not an error
                }
            } else {
                errs.mismatch(modulePath,
                              "a list of transports, or a mapping with a "
                              "\"transports\" list",
                              moduleObj);
                return false;
            }
            for (std::size_t i = 0; i < arr->size(); ++i) {
                if (!isLocalTransportEntry(
                        (*arr)[i], "protocol",
                        {"host", "port", "codec", "ca_file", "verify_peer", "cert", "key"}, errs,
                        modulePath + ".transports[" + std::to_string(i) + "]"))
                    return false;
            }
        }
    }
    if (!errs.ok()) return false;

    if (const auto ssl = obj.find("ssl"); ssl != obj.end() && !ssl->is_null()) {
        std::string key = r.path("ssl");
        bool empty = ssl->is_object();
        for (auto it = ssl->begin(); empty && it != ssl->end(); ++it) {
            if (!it->is_null() && *it != json("")) {
                key += "." + it.key();
                empty = false;
            }
        }
        if (!empty) {
            errs.note(removedWithTcpTransports(key));
            return false;
        }
    }
    if (const auto it = obj.find("insecure_tcp");
        it != obj.end() && !it->is_null() && *it != json(false)) {
        errs.note(removedWithTcpTransports(r.path("insecure_tcp")));
        return false;
    }
    return true;
}

// Inverse of daemonConfigToJson — used by both config.json and
// state.json readers (the latter parses the `resolved` block).
//
// Returns std::nullopt, with the reason in `errs`, when anything in the
// document is not what the schema expects: a value of the wrong type
// (`modules_dirs:` given a scalar), an out-of-range number, or a setting the
// tcp and tcp_ssl transports took with them. Failing the parse forces the
// operator to see the error and fix the file; nothing is applied in the
// meantime.
// `prefix` is the dotted path of `obj` inside its file — empty for config.yaml,
// "resolved." for the block state.json nests it under — so every message points
// at the key as it appears in the file the operator would open.
std::optional<DaemonConfig> daemonConfigFromJson(const json& obj,
                                                 json_schema::Errors& errs,
                                                 const std::string& prefix = {})
{
    DaemonConfig cfg;
    json_schema::Reader r(obj, errs, prefix);
    cfg.modulesDirs     = r.stringList("modules_dirs");
    cfg.persistencePath = r.str("persistence_path");

    if (!removedTransportKeysLoad(obj, r)) return std::nullopt;

    if (const json* dirs = r.mapping("dirs")) {
        json_schema::Reader d(*dirs, errs, r.path("dirs") + ".");
        cfg.dirs.modules = d.str("modules");
        cfg.dirs.plugins = d.str("plugins");
        cfg.dirs.keyring = d.str("keyring");
        cfg.dirs.data    = d.str("data");
        cfg.dirs.cache   = d.str("cache");
        cfg.dirs.logs    = d.str("logs");
    }
    // persistence_path is the older spelling of dirs.data and still works;
    // dirs.data wins when both are set.
    if (cfg.dirs.data.empty()) cfg.dirs.data = cfg.persistencePath;

    if (const json* logging = r.mapping("logging")) {
        json_schema::Reader l(*logging, errs, r.path("logging") + ".");
        cfg.logging.enabled   = l.boolean("enabled", true);
        cfg.logging.file      = l.str("file", "daemon.log");
        cfg.logging.maxSizeMb = static_cast<std::size_t>(
            l.integer("max_size_mb", 10, 0, 1024 * 1024));
        cfg.logging.maxFiles  = static_cast<std::size_t>(
            l.integer("max_files", 5, 0, 1000000));
        cfg.logging.console   = l.boolean("console", true);
    }

    cfg.accessPolicy = r.str("access_policy");
    cfg.placement    = r.str("placement");
    cfg.bundledModulesDirs = r.stringList("bundled_modules_dirs");
    cfg.accessGroup  = r.str("access_group");
    if (const json* peering = r.mapping("peering")) cfg.peering = peering->dump();

    // Strict allowlist: the value is handed to package_manager, which ignores
    // what it doesn't recognise. A typo ("required", "strict") would otherwise
    // leave the module on its default `warn` while `daemon config show` kept
    // displaying the operator's stricter intent.
    cfg.signaturePolicy = r.str("signature_policy");
    if (!cfg.signaturePolicy.empty() && !isValidSignaturePolicy(cfg.signaturePolicy)) {
        errs.note(r.path("signature_policy") + ": expected one of \"none\", "
                  "\"warn\", \"require\", but got \"" + cfg.signaturePolicy + "\".");
        return std::nullopt;
    }

    // Any type mismatch recorded above (the reader keeps the first one) makes
    // the whole document unusable: half-applying it is how an operator ends up
    // with a daemon that does not match the file they are looking at.
    if (!errs.ok()) return std::nullopt;
    return cfg;
}

// Atomic *replace* helper used for both config.json and state.json.
// Writes to <path>.tmp, applies `mode` (default 0600), then renames into
// place. The rename is the atomic step — readers either see the pre-write file
// or the new one, never a half-written state. We do NOT fsync, so
// this isn't durable across power loss (close() flushes only
// userspace buffers; OS page cache is still in flight). Callers that
// need durability would need an explicit fsync(fd) on the temp file
// plus a dir fsync after rename. Returns false on any I/O step — including a
// failed chmod, so a credential file is never published at the wrong mode.
bool atomicWriteText(const fs::path& path, const std::string& text,
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
        ofs << text;
        ofs.close();
        if (!ofs) return false;
    }
    // Apply the caller's mode to the temp file (default 0600). The rename
    // preserves it, so the destination lands at exactly `mode` regardless of
    // any pre-existing file's perms or the process umask. Fail (and drop the
    // temp) if chmod fails, rather than publishing a credential file at the
    // umask default.
    // fs::path::c_str() is const wchar_t* on Windows, so go through string().
    if (logosctl::chmodPosix(tmp.string().c_str(), mode) != 0) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    std::error_code rec;
    fs::rename(tmp, path, rec);
    if (rec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

// JSON flavour, for the machine-owned files (state.json, tokens, the auto
// token). Shares the atomic-rename + chmod path above.
bool atomicWriteJson(const fs::path& path, const json& obj,
                     mode_t mode = S_IRUSR | S_IWUSR)
{
    return atomicWriteText(path, obj.dump(4) + "\n", mode);
}

} // namespace

// -- DaemonConfigFile -------------------------------------------------------

std::optional<DaemonConfig> parseDaemonConfigDocument(const json& obj,
                                                      std::string* error)
{
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return std::optional<DaemonConfig>{};
    };
    try {
        if (!obj.is_object())
            return fail("The config document must be a mapping at the top level.");

        json_schema::Errors errs;
        json_schema::Reader r(obj, errs);
        const int64_t v = r.integer("version", 0);
        if (!errs.ok()) return fail(errs.message());
        if (v != kDaemonConfigSchemaVersion)
            return fail("unsupported schema version " + std::to_string(v) +
                        " — rewrite it with `daemon config set` (expected "
                        "version " + std::to_string(kDaemonConfigSchemaVersion) +
                        ").");

        auto cfg = daemonConfigFromJson(obj, errs);
        if (!cfg) return fail(errs.ok() ? "the document could not be read."
                                        : errs.message());
        return cfg;
    } catch (const std::exception& e) {
        // Belt and braces. Every read above is type-checked, so nothing in
        // here is supposed to throw any more — but a config file must never be
        // able to terminate the process, whatever it contains.
        return fail(std::string("the document could not be read (") + e.what() + ").");
    }
}

std::string DaemonConfigFile::filePath()
{
    return Config::daemonConfigPath();
}

std::optional<DaemonConfig> DaemonConfigFile::read()
{
    std::ifstream ifs(filePath());
    if (!ifs) return std::nullopt;

    // The file is YAML (a human writes it); the schema work below is still
    // done by parseDaemonConfigDocument, so the format change stops here.
    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string parseError;
    auto parsed = yaml_json::parse(buf.str(), &parseError);
    if (!parsed) {
        std::cerr << "DaemonConfig: " << filePath() << " is not valid YAML: "
                  << parseError << std::endl;
        return std::nullopt;
    }

    // Exactly the validation `daemon config set` runs before it writes, so a
    // document that installs is a document that boots.
    std::string error;
    auto cfg = parseDaemonConfigDocument(*parsed, &error);
    if (!cfg)
        std::cerr << "DaemonConfig: " << filePath() << ": " << error << std::endl;
    return cfg;
}

bool DaemonConfigFile::write(const DaemonConfig& cfg)
{
    json obj = daemonConfigToJson(cfg);
    obj["version"] = kDaemonConfigSchemaVersion;
    // logoscore keeps writing JSON so an existing deployment's config file
    // stays readable by the tool that wrote it; logosctl writes YAML.
    // Reading needs no branch: YAML is a superset of JSON, so the same
    // parser handles both.
    return Config::flavor() == Config::Flavor::Modern
        ? atomicWriteText(fs::path(filePath()), yaml_json::dump(obj))
        : atomicWriteJson(fs::path(filePath()), obj);
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
    if (!obj.is_object()) return state;

    // Type-checked like the operator's config file. state.json is
    // machine-written, so a mismatch here means a corrupted or hand-edited
    // file — which must still read as "no usable state", never as a crash.
    json_schema::Errors errs;
    json_schema::Reader r(obj, errs);

    state.schemaVersion = static_cast<int>(r.integer("version", 0));
    if (!errs.ok()) {
        std::cerr << "DaemonRuntimeState: " << filePath() << ": "
                  << errs.message() << std::endl;
        return DaemonRuntimeState{};
    }
    if (state.schemaVersion != kDaemonRuntimeStateSchemaVersion) {
        std::cerr << "DaemonRuntimeState: unsupported schema version "
                  << state.schemaVersion
                  << " in " << filePath()
                  << " — relaunch the daemon to regenerate (expected version "
                  << kDaemonRuntimeStateSchemaVersion << ")." << std::endl;
        return state;
    }

    state.instanceId   = r.str("instance_id");
    state.pid          = r.integer("pid", -1);
    state.startedAt    = r.str("started_at");
    state.configSource = r.str("config_source");
    if (const json* resolvedObj = r.mapping("resolved")) {
        // A daemon's own record, not operator config: an older daemon's tcp
        // listeners must not hide that it is running, so drop the removed keys.
        json current = *resolvedObj;
        for (const char* removed : {"modules", "ssl", "insecure_tcp"}) current.erase(removed);
        auto resolved = daemonConfigFromJson(current, errs, "resolved.");
        if (!resolved) {
            // Same fail-the-parse contract as DaemonConfigFile::read:
            // an invalid entry means we can't trust any of the resolved
            // block. Return an empty (fileOk=false) state so callers don't
            // act on partial data.
            std::cerr << "DaemonRuntimeState: refusing to load partial "
                      << "resolved block from " << filePath() << std::endl;
            return DaemonRuntimeState{};
        }
        state.resolved = std::move(*resolved);
    }

    // Same contract for a mistyped field anywhere above: one wrong type means
    // the file cannot be trusted as a whole, so callers get "no live daemon"
    // rather than a state half-read from it.
    if (!errs.ok()) {
        std::cerr << "DaemonRuntimeState: " << filePath() << ": "
                  << errs.message() << std::endl;
        return DaemonRuntimeState{};
    }

    state.fileOk = !state.instanceId.empty();
    return state;
}

bool DaemonRuntimeStateFile::remove()
{
    std::error_code ec;
    return fs::remove(filePath(), ec);
}

bool isValidSignaturePolicy(const std::string& policy)
{
    return policy == "none" || policy == "warn" || policy == "require";
}

// Resolve an OS group name-or-gid to a gid. Accepts an all-digits string as a
// numeric gid (with overflow/range validation), otherwise looks the name up in
// the group database.
bool resolveOsGroupGid(const std::string& spec, gid_t& out)
{
#ifdef _WIN32
    // There is no group database to resolve against, and nothing downstream
    // could use the answer: named pipes carry a security descriptor, not a
    // gid, and Windows files have no group-execute bit to grant traversal
    // with. daemon.cpp rejects --access-group up front so this is never
    // reached with a non-empty spec by the daemon; the false here keeps the
    // client-artifact path owner-only if anything else ever calls it.
    (void)spec;
    (void)out;
    return false;
#else
    if (!spec.empty() &&
        spec.find_first_not_of("0123456789") == std::string::npos) {
        errno = 0;
        char* end = nullptr;
        const unsigned long v = std::strtoul(spec.c_str(), &end, 10);
        if (errno != 0 || end == spec.c_str() || *end != '\0' ||
            v > static_cast<unsigned long>(std::numeric_limits<gid_t>::max()))
            return false;
        out = static_cast<gid_t>(v);
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
#endif
}

bool DaemonRuntimeStateFile::writeLocalClientArtifacts(
    const std::string& instanceId,
    const std::string& autoTokenRaw,
    const std::string& issuedAt,
    const std::string& accessGroup)
{
    const std::string clientDir      = Config::clientDir();
    const std::string clientCfgPath  = Config::clientConfigPath();
    const std::string autoTokenPath  = Config::clientTokenPath("auto.json");

    // Resolve the access group once. An unknown group name degrades to
    // owner-only (with a warning) rather than failing the whole boot — the
    // daemon still comes up, just not shared. (daemon.cpp validates the same
    // way before exporting the socket-perm env vars, so both halves agree.)
    gid_t groupGid = 0;
    bool  shareWithGroup = false;
    if (!accessGroup.empty()) {
        if (resolveOsGroupGid(accessGroup, groupGid)) {
            shareWithGroup = true;
        } else {
            std::cerr << "writeLocalClientArtifacts: unknown --access-group '"
                      << accessGroup << "' — client artifacts stay owner-only"
                      << std::endl;
        }
    }

    std::error_code ec;
    fs::create_directories(clientDir, ec);
    if (ec) return false;

    if (shareWithGroup) {
        // Sharing widens the config dir to be group-traversable, which would
        // also expose daemon/ (token + state filenames) unless we lock it down
        // first. Do the lockdown BEFORE widening the parent, and if it fails,
        // abandon sharing entirely (fall back to owner-only) rather than leave
        // private state reachable. daemon/ may not exist yet in isolated unit
        // tests — nothing to lock, nothing exposed.
        const std::string daemonDir = Config::daemonDir();
        if (fs::exists(daemonDir, ec)
            && logosctl::chmodPosix(daemonDir.c_str(), S_IRWXU) != 0) {
            std::cerr << "writeLocalClientArtifacts: could not lock " << daemonDir
                      << " to owner-only (" << std::strerror(errno)
                      << ") — not sharing the config dir" << std::endl;
            shareWithGroup = false;
        }
    }

    const mode_t fileMode = shareWithGroup ? static_cast<mode_t>(0640)
                                           : static_cast<mode_t>(S_IRUSR | S_IWUSR);

#ifndef _WIN32
    // Unreachable on Windows -- resolveOsGroupGid always fails there, so
    // shareWithGroup is always false -- but compiled out as well, because
    // chown() and uid_t do not exist in mingw-w64 at all.
    if (shareWithGroup) {
        // Grant the group access to just the client subtree:
        //   - client/ becomes group r-x + owned by the group;
        //   - the config dir gets group traverse (execute) so a member can
        //     reach client/ without being able to list it.
        // daemon/ was locked to owner-only just above.
        ::chown(clientDir.c_str(), static_cast<uid_t>(-1), groupGid);
        ::chmod(clientDir.c_str(), 0750);

        const std::string configDir = Config::configDir();
        struct stat cst;
        if (::stat(configDir.c_str(), &cst) == 0) {
            ::chown(configDir.c_str(), static_cast<uid_t>(-1), groupGid);
            ::chmod(configDir.c_str(), (cst.st_mode & 07777) | S_IXGRP);
        }
    }
#endif

    // client/config.json — this daemon's local dial spec, rewritten every boot
    // because the instance_id is new. The token_file an existing one names stays.
    std::string tokenFileName = "auto.json";
    if (std::ifstream ifs(clientCfgPath); ifs) {
        std::stringstream ebuf;
        ebuf << ifs.rdbuf();
        // Type-checked: a stray `token_file: 42` must not abort the daemon mid-boot.
        if (auto parsed = yaml_json::parse(ebuf.str()); parsed && parsed->is_object()) {
            json_schema::Errors ignored;
            const std::string named = json_schema::Reader(*parsed, ignored).str("token_file");
            if (!named.empty()) tokenFileName = named;
        }
    }

    json client;
    client["version"]     = 2;
    client["token_file"]  = tokenFileName;
    client["instance_id"] = instanceId;
    client["daemon"]      = {{"core_service", {{"transport", "local"}}},
                             {"capability_module", {{"transport", "local"}}}};

    // The client config carries no secret (the token lives in a separate
    // file), so it is safe to make group-readable when sharing.
    const bool wrote = Config::flavor() == Config::Flavor::Modern
        ? atomicWriteText(fs::path(clientCfgPath), yaml_json::dump(client), fileMode)
        : atomicWriteJson(fs::path(clientCfgPath), client, fileMode);
    if (!wrote) return false;
#ifndef _WIN32
    // chown/uid_t do not exist in mingw-w64; shareWithGroup is always false
    // on Windows anyway (resolveOsGroupGid refuses there).
    if (shareWithGroup)
        ::chown(clientCfgPath.c_str(), static_cast<uid_t>(-1), groupGid);
#endif

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
#ifndef _WIN32
    if (shareWithGroup)
        ::chown(autoTokenPath.c_str(), static_cast<uid_t>(-1), groupGid);
#endif
    return true;
}
