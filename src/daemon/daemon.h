#ifndef DAEMON_H
#define DAEMON_H

#include "connection_file.h"

#include <string>
#include <vector>

class Daemon {
public:
    static int start(int argc, char* argv[],
                     const std::vector<std::string>& modulesDirs,
                     const std::string& persistencePath = "",
                     const std::vector<TransportInfo>& transports = {});

private:
    static void setupSignalHandlers();
    static void signalHandler(int signal);
};

#endif // DAEMON_H
