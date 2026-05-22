#include "stop_command.h"
#include <fmt/format.h>

int StopCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosMap result = client().shutdown();

    std::string status = result.value("status", std::string{});
    if (status == "error") {
        output().printError(result.value("code", std::string{}),
                            result.value("message", std::string{}), result);
        return 3;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(fmt::format("Daemon shutdown initiated: {}",
                                      result.value("message", std::string{"ok"})));
    }

    return 0;
}
