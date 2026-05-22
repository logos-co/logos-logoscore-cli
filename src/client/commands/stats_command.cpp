#include "stats_command.h"
#include <CLI/CLI.hpp>

int StatsCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"stats"};
    cli.set_help_flag();
    try {
        auto argsCopy = args;
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logoscore stats");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    LogosList stats = client().getModuleStats();
    output().printStats(stats);
    return 0;
}
