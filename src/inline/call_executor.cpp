#include "call_executor.h"
#include "logos_sdk_c.h"
#include "../string_utils.h"
#include <logos_json.h>
#include <fmt/format.h>
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
#include <sstream>

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
    nlohmann::json paramsArray = nlohmann::json::array();

    for (size_t i = 0; i < params.size(); ++i) {
        const std::string& rawParam     = params[i];
        const std::string resolvedParam = resolveParam(rawParam);

        if (strutil::starts_with(rawParam, '@') && resolvedParam.empty()) {
            fprintf(stderr, "Warning: failed to resolve file parameter: %s\n",
                    rawParam.c_str());
            return {};
        }

        nlohmann::json paramObj;
        paramObj["name"] = fmt::format("arg{}", i);

        std::string type;
        std::string lower = strutil::to_lower(resolvedParam);
        if (lower == "true" || lower == "false") {
            type = "bool";
        } else {
            // std::stoi/std::stod parse a leading prefix and ignore the rest,
            // so "1.25" would be classified as int. Require the whole string to
            // be consumed before accepting it as that type.
            bool isInt = false;
            try { size_t pos = 0; std::stoi(resolvedParam, &pos); isInt = (pos == resolvedParam.size()); } catch (...) {}
            if (isInt) {
                type = "int";
            } else {
                bool isDouble = false;
                try { size_t pos = 0; std::stod(resolvedParam, &pos); isDouble = (pos == resolvedParam.size()); } catch (...) {}
                type = isDouble ? "double" : "string";
            }
        }

        paramObj["type"]  = type;
        paramObj["value"] = resolvedParam;
        paramsArray.push_back(paramObj);
    }

    return paramsArray.dump();
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

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!result.completed && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

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
