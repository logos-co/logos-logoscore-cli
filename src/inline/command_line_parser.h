#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <string>
#include <vector>

struct ModuleCall {
    std::string moduleName;
    std::string methodName;
    std::vector<std::string> params;  // Raw param strings (including @file refs)
};

struct CoreArgs {
    bool valid;
    bool quitOnFinish;                          // Exit after -c calls complete (--quit-on-finish)
    std::vector<std::string> modulesDirs;       // Optional: custom modules directories (repeatable -m)
    std::vector<std::string> loadModules;       // Optional: modules to load in order
    std::vector<ModuleCall> calls;              // Optional: module method calls to execute
};

// Parse a "module.method(arg1,arg2)" call string into a ModuleCall struct.
ModuleCall parseCallString(const std::string& callStr);

#endif // COMMAND_LINE_PARSER_H
