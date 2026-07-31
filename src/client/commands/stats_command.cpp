#include "stats_command.h"
#include <CLI/CLI.hpp>

int StatsCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"stats"};
    cli.set_help_flag();
    try {
        parseArgs(cli, args);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logosctl stats");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosList stats = client().getModuleStats();
    output().printStats(stats);
    return 0;
}
