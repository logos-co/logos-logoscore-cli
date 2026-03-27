#include "stats_command.h"

int StatsCommand::execute(const std::vector<std::string>& args)
{
    (void)args;

    int err = ensureConnected();
    if (err != 0)
        return err;

    QJsonArray stats = client().getModuleStats();

    output().printStats(stats);
    return 0;
}
