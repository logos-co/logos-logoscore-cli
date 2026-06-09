#include "call_command.h"
#include "../../string_utils.h"
#include <fmt/format.h>
#include <fstream>
#include <sstream>

std::string CallCommand::resolveFileParam(const std::string& param)
{
    if (!strutil::starts_with(param, '@'))
        return param;

    std::string filePath = param.substr(1);
    std::ifstream file(filePath);
    if (!file.is_open())
        return {};

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

int CallCommand::execute(const std::vector<std::string>& args)
{
    std::string moduleName;
    std::string methodName;
    std::vector<std::string> methodArgs;

    if (args.empty()) {
        output().printError("INVALID_ARGS",
                            "Usage: logoscore call <module> <method> [args...]");
        return 1;
    }

    if (args.size() >= 3 && args[1] == "method") {
        moduleName = args[0];
        methodName = args[2];
        for (size_t j = 3; j < args.size(); ++j)
            methodArgs.push_back(args[j]);
    } else {
        if (args.size() < 2) {
            output().printError("INVALID_ARGS",
                                "Usage: logoscore call <module> <method> [args...]");
            return 1;
        }
        moduleName = args[0];
        methodName = args[1];
        for (size_t j = 2; j < args.size(); ++j)
            methodArgs.push_back(args[j]);
    }

    if (moduleName.empty() || methodName.empty()) {
        output().printError("INVALID_ARGS",
                            "Usage: logoscore call <module> <method> [args...]");
        return 1;
    }

    int err = ensureConnected();
    if (err != 0)
        return err;

    // Resolve @file parameters and coerce types to native JSON values
    LogosList resolvedArgs = LogosList::array();
    for (const std::string& arg : methodArgs) {
        std::string resolved = resolveFileParam(arg);
        if (strutil::starts_with(arg, '@') && resolved.empty()) {
            output().printError("INVALID_ARGS",
                                fmt::format("Failed to read file: {}", arg.substr(1)));
            return 1;
        }

        if (resolved == "true") {
            resolvedArgs.push_back(true);
        } else if (resolved == "false") {
            resolvedArgs.push_back(false);
        } else {
            // std::stoi/std::stod parse a leading prefix and ignore the rest,
            // so "1.25" would parse as int 1. Require the whole string to be
            // consumed (matching the old QString::toInt(&ok) semantics) before
            // accepting it as that type.
            bool isInt = false;
            int intVal = 0;
            try {
                size_t pos = 0;
                intVal = std::stoi(resolved, &pos);
                isInt = (pos == resolved.size());
            } catch (...) {}

            if (isInt) {
                resolvedArgs.push_back(intVal);
            } else {
                bool isDouble = false;
                double dblVal = 0.0;
                try {
                    size_t pos = 0;
                    dblVal = std::stod(resolved, &pos);
                    isDouble = (pos == resolved.size());
                } catch (...) {}

                if (isDouble) {
                    resolvedArgs.push_back(dblVal);
                } else {
                    resolvedArgs.push_back(resolved);
                }
            }
        }
    }

    LogosMap result = client().callModuleMethod(moduleName, methodName, resolvedArgs);

    std::string status = result.value("status", std::string{});
    if (status == "error") {
        std::string code = result.value("code", std::string{});
        int exitCode = 4;
        if (code == "MODULE_NOT_LOADED" || code == "MODULE_NOT_FOUND")
            exitCode = 3;
        output().printError(code, result.value("message", std::string{}), result);
        return exitCode;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        const auto& resultValue = result["result"];
        if (resultValue.is_string()) {
            output().printRaw(resultValue.get<std::string>());
        } else if (resultValue.is_number_float()) {
            double d = resultValue.get<double>();
            if (d == static_cast<double>(static_cast<int64_t>(d)))
                output().printRaw(fmt::format("{}", static_cast<int64_t>(d)));
            else
                output().printRaw(fmt::format("{}", d));
        } else if (resultValue.is_number_integer()) {
            output().printRaw(fmt::format("{}", resultValue.get<int64_t>()));
        } else if (resultValue.is_boolean()) {
            output().printRaw(resultValue.get<bool>() ? "true" : "false");
        } else if (resultValue.is_null()) {
            // Nothing useful to print
        } else {
            output().printRaw(resultValue.dump(2));
        }
    }

    return 0;
}
