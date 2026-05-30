#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <functional>
#include <string>

class EventLoop {
public:
    static void init(int argc, char* argv[],
                     const std::string& appName,
                     const std::string& appVersion);
    static int exec();
    static void quit();
    static void onAboutToQuit(std::function<void()> callback);
    static std::string applicationVersion();
    static void installLogFilter(bool verbose);
};

#endif // EVENT_LOOP_H
