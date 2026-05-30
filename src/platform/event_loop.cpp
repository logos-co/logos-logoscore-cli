#include "event_loop.h"
#include <logos_event_loop.h>

void EventLoop::init(int argc, char* argv[],
                     const std::string& appName,
                     const std::string& appVersion)
{
    logos_event_loop_init(argc, argv, appName.c_str(), appVersion.c_str());
}

int EventLoop::exec()
{
    return logos_event_loop_exec();
}

void EventLoop::quit()
{
    logos_event_loop_quit();
}

void EventLoop::onAboutToQuit(std::function<void()> callback)
{
    static std::function<void()> s_callback;
    s_callback = std::move(callback);
    logos_event_loop_on_about_to_quit(
        [](void*) { s_callback(); }, nullptr);
}

std::string EventLoop::applicationVersion()
{
    const char* v = logos_event_loop_app_version();
    return v ? std::string(v) : std::string();
}

void EventLoop::installLogFilter(bool verbose)
{
    logos_event_loop_install_log_filter(verbose ? 1 : 0);
}
