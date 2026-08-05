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
    // `persistConfig` writes the merged config back to disk. Only the legacy
    // logoscore front-end passes it: logosctl has no merge layer to persist,
    // since its config file *is* the source of truth.
    static int start(int argc, char* argv[],
                     const DaemonConfig& cfg,
                     const std::string& configSource,
                     // No default arguments. They are both bool and adjacent,
                     // so a defaulted tail let a caller drop one and have the
                     // other silently slide into its place -- which is exactly
                     // what happened: logosctl passed g_verbose as
                     // persistConfig for the whole life of the binary, and it
                     // compiled. Requiring both makes that a build error.
                     bool persistConfig,
                     bool verbose);

private:
    static void setupSignalHandlers();
    static void signalHandler(int signal);
};

#endif // DAEMON_H
