#include "call_executor.h"
#include "logos_sdk_c.h"
#include "../string_utils.h"
#include <QEventLoop>
#include <QTimer>
#include <QObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

struct CallResult {
    bool completed = false;
    bool success   = false;
    std::string message;
};

static void methodCallCallback(int result, const char* message, void* user_data) {
    CallResult* callResult = static_cast<CallResult*>(user_data);
    callResult->completed = true;
    callResult->success   = (result == 1);
    callResult->message   = message ? std::string(message) : std::string();
}

std::string CallExecutor::resolveParam(const std::string& param) {
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

std::string CallExecutor::buildParamsJson(const std::vector<std::string>& params) {
    QJsonArray paramsArray;

    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& rawParam      = params[i];
        const std::string resolvedParam  = resolveParam(rawParam);

        if (strutil::starts_with(rawParam, '@') && resolvedParam.empty()) {
            fprintf(stderr, "Warning: failed to resolve file parameter: %s\n",
                    rawParam.c_str());
            return {};
        }

        QJsonObject paramObj;
        paramObj["name"] = QString::fromStdString(fmt::format("arg{}", i));

        // Determine type and coerce value
        std::string type;
        std::string lower = strutil::to_lower(resolvedParam);
        if (lower == "true" || lower == "false") {
            type = "bool";
        } else {
            bool isInt = false;
            try { std::stoi(resolvedParam); isInt = true; } catch (...) {}
            if (isInt) {
                type = "int";
            } else {
                bool isDouble = false;
                try { std::stod(resolvedParam); isDouble = true; } catch (...) {}
                type = isDouble ? "double" : "string";
            }
        }

        paramObj["type"]  = QString::fromStdString(type);
        paramObj["value"] = QString::fromStdString(resolvedParam);
        paramsArray.append(paramObj);
    }

    QJsonDocument doc(paramsArray);
    return doc.toJson(QJsonDocument::Compact).toStdString();
}

bool CallExecutor::executeCall(const ModuleCall& call) {
    std::string paramsJson = buildParamsJson(call.params);
    if (call.params.size() > 0 && paramsJson.empty()) {
        std::cerr << "Error: Failed to build parameters for "
                  << call.moduleName << "."
                  << call.methodName << std::endl;
        return false;
    }

    CallResult result;

    logos_sdk_call_method_async(
        call.moduleName.c_str(),
        call.methodName.c_str(),
        paramsJson.empty() ? "[]" : paramsJson.c_str(),
        methodCallCallback,
        &result
    );

    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    timeoutTimer.setInterval(30000);

    QTimer pollTimer;
    pollTimer.setInterval(100);

    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&pollTimer, &QTimer::timeout, [&]() {
        if (result.completed)
            loop.quit();
    });

    timeoutTimer.start();
    pollTimer.start();
    loop.exec();

    pollTimer.stop();
    timeoutTimer.stop();

    if (!result.completed) {
        std::cerr << "Error: Timeout waiting for "
                  << call.moduleName << "." << call.methodName << std::endl;
        return false;
    }

    if (!result.success) {
        std::cerr << "Error: " << result.message << std::endl;
        return false;
    }

    std::cout << result.message << std::endl;
    return true;
}

int CallExecutor::executeCalls(const std::vector<ModuleCall>& calls) {
    for (const ModuleCall& call : calls) {
        if (!executeCall(call))
            return 1;
    }
    return 0;
}
