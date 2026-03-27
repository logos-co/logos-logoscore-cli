#ifndef DAEMON_H
#define DAEMON_H

#include <string>
#include <vector>

class Daemon {
public:
    static int start(int argc, char* argv[], const std::vector<std::string>& modulesDirs);

private:
    static void setupSignalHandlers();
    static void signalHandler(int signal);
};

#endif // DAEMON_H
