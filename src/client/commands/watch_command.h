#ifndef WATCH_COMMAND_H
#define WATCH_COMMAND_H

#include "command.h"

#include <condition_variable>
#include <functional>
#include <mutex>

// Blocks until `stopped()` holds. A wakeup that finds it false, notified or
// spurious, waits again.
void waitForStop(std::mutex& mutex, std::condition_variable& wake,
                 const std::function<bool()>& stopped);

class WatchCommand : public Command {
public:
    using Command::Command;

    int execute(const std::vector<std::string>& args) override;
    std::string name() const override { return "watch"; }
    std::string description() const override { return "Watch events from a module"; }
};

#endif // WATCH_COMMAND_H
