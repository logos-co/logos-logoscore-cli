#include "list_modules_command.h"
#include <CLI/CLI.hpp>

int ListModulesCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"list-modules"};
    cli.set_help_flag();
    bool loadedOnly = false;
    cli.add_flag("--loaded", loadedOnly, "Show only loaded modules");
    try {
        parseArgs(cli, args);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS", "Usage: logosctl module ls [--loaded]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    std::string filter = loadedOnly ? "loaded" : "all";
    const std::optional<LogosList> modules = client().listModules(filter);

    // An unanswered RPC is not an empty module list. This printed `[]` and
    // exited 0 against a daemon that was not running, which is the worst
    // possible answer: a script cannot tell it apart from a healthy session
    // with nothing loaded, so "no modules found" silently became a fact.
    if (!modules) {
        output().printError("DAEMON_UNREACHABLE",
                            "The daemon did not answer, so the module list is "
                            "unknown (this is not an empty list).");
        return 2;
    }

    output().printModuleList(*modules);
    return 0;
}
