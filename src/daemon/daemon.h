#ifndef DAEMON_H
#define DAEMON_H

#include "daemon_state.h"

#include <map>
#include <string>
#include <vector>

class Daemon {
public:
    // Boot the daemon from a fully-merged DaemonConfig. The caller (in
    // main.cpp) is responsible for the merge order (`defaults <
    // config.json < CLI args`) and per-flag override detection via
    // CLI11 `Option::count()`. Daemon::start treats `cfg` as
    // authoritative — it doesn't re-read disk files or re-apply
    // defaults.
    //
    // `cfg.modules` must already include `core_service` and
    // `capability_module` entries; the caller defaults them to
    // LocalSocket-only when neither config.json nor CLI flags
    // populated them. For non-LocalSocket entries with `port == 0`,
    // the daemon pre-allocates a fresh ephemeral port via
    // `PortAllocator` so the listener doesn't race the kernel for it.
    //
    // `configSource` is a diagnostic string ("cli" | "config.json" |
    // "defaults") recorded into state.json's `config_source` field
    // so operators can tell at a glance where the running daemon's
    // config came from.
    // If `persistConfig` is true, the daemon writes `cfg` to
    // daemon/config.json after transports have successfully bound
    // and state.json is on disk. The persisted file holds operator
    // *intent* (port=0 stays 0) — the actually-bound port lives in
    // state.json. No-op when false, which is the default outcome of
    // a launch that didn't pass `--persist-config`.
    static int start(int argc, char* argv[],
                     const DaemonConfig& cfg,
                     const std::string& configSource,
                     bool persistConfig,
                     bool verbose = false);

private:
    static void setupSignalHandlers();
    static void signalHandler(int signal);
};

#endif // DAEMON_H
