#ifndef DAEMON_H
#define DAEMON_H

#include "daemon_state.h"

#include <map>
#include <string>
#include <vector>

class Daemon {
public:
    // `moduleTransports` is the daemon's per-module transport
    // configuration as parsed from `--module-transport` CLI flags.
    // Each entry is the full list of listeners for that module; the
    // map is authoritative — there is no implicit inheritance from
    // any "source" module. Callers populate well-known modules
    // (`core_service`, `capability_module`) before calling; missing
    // entries default to LocalSocket-only inside `start`.
    //
    // For non-LocalSocket entries with `port == 0`, the daemon
    // pre-allocates a fresh ephemeral port via `PortAllocator` so the
    // listener doesn't race the kernel for it.
    static int start(int argc, char* argv[],
                     const std::vector<std::string>& modulesDirs,
                     const std::string& persistencePath,
                     const std::map<std::string,
                                    std::vector<TransportInfo>>&
                         moduleTransports);

private:
    static void setupSignalHandlers();
    static void signalHandler(int signal);
};

#endif // DAEMON_H
