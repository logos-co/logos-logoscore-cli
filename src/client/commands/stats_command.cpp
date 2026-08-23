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

    const std::optional<LogosList> stats = client().getModuleStats();

    // Same reasoning as `module ls`: no reply is not "no modules to report
    // on". See Client::getModuleStats.
    if (!stats) {
        output().printError("DAEMON_UNREACHABLE",
                            "The daemon did not answer, so module stats are "
                            "unknown (this is not an empty list).");
        return 2;
    }

    output().printStats(*stats);
    return 0;
}
