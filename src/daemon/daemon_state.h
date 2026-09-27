#ifndef DAEMON_STATE_H
#define DAEMON_STATE_H

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <sys/types.h>   // gid_t

#ifdef _WIN32
// mingw-w64's sys/types.h has no gid_t — Windows identifies principals by SID,
// not by a small integer. Declared so the signatures below still compile; the
// only producer, resolveOsGroupGid, always fails on Windows, so no value of
// this type is ever meaningful there.
using gid_t = unsigned int;
#endif

// `daemon/config.json` (operator preferences) and `daemon/state.json`
// (live runtime state). Both use the same v2 schema number — the
// previous unified daemon/daemon.json v2 was internal-only, so the version
// is reused for the split layout.
constexpr int kDaemonConfigSchemaVersion       = 2;
constexpr int kDaemonRuntimeStateSchemaVersion = 2;

// Redirects for the session's subdirectories. Empty means "use the default",
// which is <configDir>/<name> -- that default is what makes a session portable.
// Set one to share a keyring between sessions, put the cache on a bigger disk,
// or point at a modules tree something else manages.
struct SessionDirs {
    std::string modules;
    std::string plugins;
    std::string keyring;
    std::string data;
    std::string cache;
    std::string logs;
};

// Daemon log file settings. Capture is pipe-based so module subprocesses are
// included; see src/daemon/log_sink.h.
struct LoggingConfig {
    bool        enabled   = true;
    std::string file      = "daemon.log";
    // Rotate past this size, keeping maxFiles in total. 0 = never rotate.
    std::size_t maxSizeMb = 10;
    std::size_t maxFiles  = 5;
    // Mirror to the terminal too. Ignored by a --detach daemon, which has none.
    bool console = true;
};

// Operator-typed preferences. Persists to daemon/config.json on
// `--persist-config`. Defaults supplied by the merge layer in main.
struct DaemonConfig {
    std::vector<std::string> modulesDirs;
    std::string              persistencePath;
    SessionDirs              dirs;
    LoggingConfig            logging;
    // Package signature enforcement, pushed into the bundled
    // package_manager module at boot the same way the keyring directory is.
    // One of "none" | "warn" | "require"; empty means "say nothing and let
    // the module keep its own default" (which is warn). Any other value is
    // rejected by the config reader rather than silently ignored.
    std::string signaturePolicy;
    // Inter-module access policy: resolved JSON text (from --access-policy
    // file or inline). Empty means none. Persisted across launches.
    std::string accessPolicy;
    // Where modules run, as the runtime's placement policy JSON (--placement).
    // Empty keeps the runtime's default. Persisted across launches.
    std::string placement;
    // Directories whose modules count as bundled too (--bundled-modules-dir):
    // a reserved name may come from them, and their modules may run in-process.
    std::vector<std::string> bundledModulesDirs;
    // OS group to share the daemon with (from --access-group). When set, the
    // daemon (1) exports LOGOS_SOCKET_GROUP + LOGOS_SOCKET_MODE=0660 so every
    // module/host process chgrp's + widens its local socket for the group, and
    // (2) makes the client artifacts (client/, client/config.json,
    // client/auto.json) group-readable and chgrp's them, so a second OS user in
    // the group can drive the daemon. Empty means owner-only (the default).
    // Persisted across launches.
    std::string accessGroup;
    // Links with other runtimes (the `peering` section) as JSON text, handed to
    // peering_module unchanged, which refuses what it does not know. Empty
    // when absent. Persisted across launches.
    std::string peering;
};

// Live-instance runtime state. Written to daemon/state.json on every
// successful boot (after transports actually bind), removed at clean
// shutdown. Stale state.json after a crash is tolerable: the next
// boot overwrites it; co-resident clients can detect "no live daemon"
// by `kill(state.pid, 0) == ESRCH`.
struct DaemonRuntimeState {
    bool fileOk = false;
    int  schemaVersion = 0;

    // Ephemeral identity for this process.
    std::string instanceId;
    int64_t     pid = -1;
    std::string startedAt;
    // Diagnostic: which layer supplied the highest-precedence value
    // for this run. One of "cli", "config.json", "defaults".
    std::string configSource;

    // The merged config this daemon runs with.
    DaemonConfig resolved;
};

// Format the current UTC time as ISO 8601 (e.g. "2026-04-28T12:34:56Z").
// Exposed so daemon.cpp can stamp `started_at` without duplicating the
// chrono boilerplate.
std::string currentUtcIso8601();

// Resolve an OS group name-or-numeric-gid to a gid. Returns false on an unknown
// name or an out-of-range numeric value. Shared by the daemon (to validate
// --access-group before exporting the socket-perm env vars) and the client
// artifact writer, so both apply exactly the same policy.
bool resolveOsGroupGid(const std::string& spec, gid_t& out);

// The accepted `signature_policy:` values. Shared by the config reader (which
// rejects anything else) and by callers that want to check a value without
// hard-coding the list.
bool isValidSignaturePolicy(const std::string& policy);

// Validate a daemon-config document (YAML already converted to JSON) and turn
// it into a DaemonConfig, without touching disk. Returns nullopt with a
// one-line reason in `error` for anything the daemon could not boot from: a
// wrong top-level type, an unsupported schema version, a value of the wrong
// type (`modules_dirs:` given a scalar), an out-of-range number, or a setting
// removed with the tcp and tcp_ssl transports (whose defaults still load). The
// message names the offending key by its dotted path.
//
// Never throws: a hand-written config must not be able to terminate the
// process. This is the same entry point DaemonConfigFile::read uses, so
// `daemon config set` can validate a document *before* writing it and know
// that anything it accepts is a document the daemon will accept too.
std::optional<DaemonConfig> parseDaemonConfigDocument(const nlohmann::json& obj,
                                                      std::string* error);

// daemon/config.json — operator preferences. read() returns nullopt
// when the file is missing, or does not load (the reason on stderr); the
// daemon starts on defaults only in the first case. write() is atomic
// (write-temp + rename) and creates the parent dir.
class DaemonConfigFile {
public:
    static std::string filePath();
    static std::optional<DaemonConfig> read();
    static bool write(const DaemonConfig& cfg);
};

// daemon/state.json — live runtime state. Written on every boot,
// removed at shutdown. read()'s `fileOk` flag is true iff a daemon
// has announced itself (instance_id populated). remove() is the
// shutdown hook.
class DaemonRuntimeStateFile {
public:
    static std::string filePath();
    static DaemonRuntimeState read();
    static bool write(const DaemonRuntimeState& state);
    static bool remove();

    // Emit <configDir>/client/config.json + <configDir>/client/auto.json:
    // the local dial spec for the daemon we just started and its boot token.
    // Called by the daemon at boot after auto-issuing the `auto` token, so
    // `logosctl status` (and friends) work out of the box on the same machine.
    //
    // Both are rewritten every boot, as the instance_id and the auto token
    // change each time. An existing client/config.json keeps its `token_file`.
    //
    // `autoTokenRaw` is the raw token returned from
    // TokenStore::issueToken — written verbatim into client/auto.json
    // so a `cp` operation could replicate it cleanly.
    //
    // When `accessGroup` is non-empty the emitted client dir + files are made
    // group-readable (client/ 0750, config.json / auto.json 0640) and chgrp'd
    // to that group, and the config dir is made group-traversable, so a second
    // OS user in the group can read them. Empty keeps the owner-only default.
    static bool writeLocalClientArtifacts(const std::string& instanceId,
                                          const std::string& autoTokenRaw,
                                          const std::string& issuedAt,
                                          const std::string& accessGroup = {});
};

#endif // DAEMON_STATE_H
