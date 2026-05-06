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
    // `hasQuoted` tracks whether a quoted fragment contributed to the
    // current arg — even if empty (`""`). Without this flag we'd have no
    // way to tell a user-intended empty string apart from an unfilled
    // position between commas, so every `concat("", "")` would collapse
    // into zero args. Resets on each comma.
    bool hasQuoted = false;
    // `seenComma` tracks whether we've passed at least one separator —
    // if so, the params list has declared N slots and we should preserve
    // empty-between-commas as empty strings rather than swallowing them.
    bool seenComma = false;

    auto pushArg = [&]() {
        std::string trimmed = trim(current);
        if (!trimmed.empty() || hasQuoted || seenComma) {
            result.push_back(trimmed);
        }
        current.clear();
        hasQuoted = false;
    };

    for (size_t i = 0; i < paramsStr.length(); ++i) {
        char c = paramsStr[i];

        if (!inQuote) {
            if (c == '\'' || c == '"') {
                inQuote = true;
                quoteChar = c;
                hasQuoted = true;
            } else if (c == ',') {
                // Set `seenComma` BEFORE flushing so the segment we
                // just consumed (which may be empty for a leading
                // comma like `,a` or back-to-back `,,`) gets pushed
                // as an explicit empty arg. Setting after pushArg
                // would drop the *first* empty position in those
                // cases — e.g. `,a` becomes ["a"] instead of ["", "a"].
                seenComma = true;
                pushArg();
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

    // Flush the trailing arg. The top-level "is there anything?" check
    // avoids pushing a phantom arg for completely empty input (`fn()`).
    if (!current.empty() || hasQuoted || seenComma) {
        pushArg();
    }

    return result;
}

ModuleCall parseCallString(const std::string& callStr) {
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
