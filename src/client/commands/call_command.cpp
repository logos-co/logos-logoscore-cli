#include "call_command.h"
#include "../../string_utils.h"
#include <QJsonDocument>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

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

    // Check for verbose "module <name> method <method>" syntax
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

    // Resolve @file parameters and coerce types
    QVariantList resolvedArgs;
    for (const std::string& arg : methodArgs) {
        std::string resolved = resolveFileParam(arg);
        if (strutil::starts_with(arg, '@') && resolved.empty()) {
            output().printError("INVALID_ARGS",
                                fmt::format("Failed to read file: {}", arg.substr(1)));
            return 1;
        }

        // Coerce to native types so the RPC target can match signatures
        if (resolved == "true") {
            resolvedArgs.append(true);
        } else if (resolved == "false") {
            resolvedArgs.append(false);
        } else {
            bool isInt = false;
            int intVal = 0;
            try { intVal = std::stoi(resolved); isInt = true; }
            catch (...) {}

            if (isInt) {
                resolvedArgs.append(intVal);
            } else {
                bool isDouble = false;
                double dblVal = 0.0;
                try { dblVal = std::stod(resolved); isDouble = true; }
                catch (...) {}

                if (isDouble) {
                    resolvedArgs.append(dblVal);
                } else {
                    resolvedArgs.append(QString::fromStdString(resolved));
                }
            }
        }
    }

    QJsonObject result = client().callModuleMethod(moduleName, methodName, resolvedArgs);

    std::string status = result.value("status").toString().toStdString();
    if (status == "error") {
        std::string code = result.value("code").toString().toStdString();
        int exitCode = 4;
        if (code == "MODULE_NOT_LOADED" || code == "MODULE_NOT_FOUND")
            exitCode = 3;
        output().printError(code, result.value("message").toString().toStdString(), result);
        return exitCode;
    }

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        QJsonValue resultValue = result.value("result");
        if (resultValue.isString()) {
            output().printRaw(resultValue.toString().toStdString());
        } else if (resultValue.isDouble()) {
            double d = resultValue.toDouble();
            if (d == static_cast<int>(d))
                output().printRaw(fmt::format("{}", static_cast<int>(d)));
            else
                output().printRaw(fmt::format("{}", d));
        } else if (resultValue.isBool()) {
            output().printRaw(resultValue.toBool() ? "true" : "false");
        } else if (resultValue.isNull() || resultValue.isUndefined()) {
            // Nothing useful to print
        } else if (resultValue.isArray()) {
            QJsonDocument doc(resultValue.toArray());
            output().printRaw(doc.toJson(QJsonDocument::Indented).toStdString());
        } else if (resultValue.isObject()) {
            QJsonDocument doc(resultValue.toObject());
            output().printRaw(doc.toJson(QJsonDocument::Indented).toStdString());
        }
    }

    return 0;
}
