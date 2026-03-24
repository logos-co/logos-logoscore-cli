#include "stats_command.h"

int StatsCommand::execute(const QStringList& args)
{
    Q_UNUSED(args);

    int err = ensureConnected();
    if (err != 0)
        return err;

    QJsonArray stats = client().getModuleStats();

    output().printStats(stats);
    return 0;
}
