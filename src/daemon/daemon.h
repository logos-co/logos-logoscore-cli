#ifndef DAEMON_H
#define DAEMON_H

#include <QStringList>

class Daemon {
public:
    static int start(int argc, char* argv[], const QStringList& modulesDirs);

private:
    static void setupSignalHandlers();
    static void signalHandler(int signal);
};

#endif // DAEMON_H
