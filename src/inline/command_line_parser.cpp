#include "command_line_parser.h"
#include <cstdio>
#include <string>

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> parseParams(const std::string& paramsStr) {
    std::vector<std::string> result;
    std::string current;
    char quoteChar = 0;
    bool inQuote = false;

    for (size_t i = 0; i < paramsStr.length(); ++i) {
        char c = paramsStr[i];

        if (!inQuote) {
            if (c == '\'' || c == '"') {
                inQuote = true;
                quoteChar = c;
            } else if (c == ',') {
                std::string trimmed = trim(current);
                if (!trimmed.empty()) {
                    result.push_back(trimmed);
                }
                current.clear();
            } else {
                current += c;
            }
        } else {
            if (c == quoteChar) {
                inQuote = false;
            } else {
                current += c;
            }
        }
    }

    std::string trimmed = trim(current);
    if (!trimmed.empty()) {
        result.push_back(trimmed);
    }

    return result;
}

static ModuleCall parseCallString(const std::string& callStr) {
    ModuleCall call;

    auto dotIndex = callStr.find('.');
    if (dotIndex == std::string::npos) {
        fprintf(stderr, "Invalid call syntax (no dot found): %s\n", callStr.c_str());
        return call;
    }

    call.moduleName = trim(callStr.substr(0, dotIndex));

    auto parenStart = callStr.find('(', dotIndex);
    if (parenStart == std::string::npos) {
        call.methodName = trim(callStr.substr(dotIndex + 1));
        return call;
    }

    call.methodName = trim(callStr.substr(dotIndex + 1, parenStart - dotIndex - 1));

    auto parenEnd = callStr.rfind(')');
    if (parenEnd == std::string::npos || parenEnd <= parenStart) {
        fprintf(stderr, "Invalid call syntax (mismatched parentheses): %s\n", callStr.c_str());
        return call;
    }

    std::string paramsStr = callStr.substr(parenStart + 1, parenEnd - parenStart - 1);
    call.params = parseParams(paramsStr);

    return call;
}

CoreArgs parseCommandLineArgs(int argc, char* argv[]) {
    CoreArgs args;
    args.valid = false;
    args.quitOnFinish = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "-m" || arg == "--modules-dir") {
            if (i + 1 < argc) {
                args.modulesDirs.push_back(argv[++i]);
            }
        } else if (arg == "-l" || arg == "--load-modules") {
            if (i + 1 < argc) {
                std::string modulesList(argv[++i]);
                std::string current;
                for (char c : modulesList) {
                    if (c == ',') {
                        if (!current.empty()) {
                            args.loadModules.push_back(current);
                            current.clear();
                        }
                    } else {
                        current += c;
                    }
                }
                if (!current.empty()) {
                    args.loadModules.push_back(current);
                }
            }
        } else if (arg == "-c" || arg == "--call") {
            if (i + 1 < argc) {
                std::string callStr(argv[++i]);
                ModuleCall call = parseCallString(callStr);
                if (!call.moduleName.empty() && !call.methodName.empty()) {
                    args.calls.push_back(call);
                } else {
                    fprintf(stderr, "Skipping invalid call: %s\n", callStr.c_str());
                }
            }
        } else if (arg == "--quit-on-finish") {
            args.quitOnFinish = true;
        }
        // --verbose, --help, --version handled elsewhere
    }

    args.valid = true;
    return args;
}
