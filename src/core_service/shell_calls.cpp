#include "shell_calls.h"

#include "logos_core.h"

namespace shell_calls {
namespace {

// core_service deadlines: a load waits out its modules' bring-up.
constexpr int kLifecycleMs = 120000;
constexpr int kQueryMs = 15000;

std::mutex& bindingMutex()
{
    static std::mutex mutex;
    return mutex;
}

logos_consumer*& binding()
{
    static logos_consumer* value = nullptr;
    return value;
}

bool answeredOk(const nlohmann::json& answer)
{
    return answer.is_object() && answer.value("status", std::string{}) == "ok";
}

} // namespace

void install(logos_consumer* value)
{
    std::lock_guard<std::mutex> lock(bindingMutex());
    binding() = value;
}

void release()
{
    // No package operation is using it once this holds.
    std::lock_guard<std::mutex> operations(packageOperations());
    logos_consumer* value = nullptr;
    {
        std::lock_guard<std::mutex> lock(bindingMutex());
        value = binding();
        binding() = nullptr;
    }
    if (value) logos_consumer_release(value);
}

nlohmann::json call(const char* method, const nlohmann::json& args, int timeoutMs)
{
    logos_consumer* shell = nullptr;
    {
        std::lock_guard<std::mutex> lock(bindingMutex());
        shell = binding();
    }
    if (!shell) return nullptr;
    char* result = nullptr;
    char* error = nullptr;
    const int status = logos_consumer_call(shell, "core_service", method, args.dump().c_str(),
                                           timeoutMs, &result, &error);
    nlohmann::json value = status == 0 && result
        ? nlohmann::json::parse(result, nullptr, /*allow_exceptions=*/false)
        : nlohmann::json();
    logos_consumer_string_free(result);
    logos_consumer_string_free(error);
    return value.is_discarded() ? nlohmann::json() : value;
}

bool load(const std::string& name)
{
    return answeredOk(call("loadModule", nlohmann::json::array({name, "required_and_optional"}),
                           kLifecycleMs));
}

bool unload(const std::string& name, bool withDependents)
{
    return answeredOk(call("unloadModule", nlohmann::json::array({name, withDependents}),
                           kLifecycleMs));
}

void refresh()
{
    call("refreshModules", nlohmann::json::array(), kLifecycleMs);
}

std::vector<std::string> loaded()
{
    std::vector<std::string> names;
    const nlohmann::json listed = call("listModules", nlohmann::json::array({"loaded"}), kQueryMs);
    if (!listed.is_array()) return names;
    for (const auto& entry : listed)
        if (entry.is_object() && entry.contains("name") && entry["name"].is_string())
            names.push_back(entry["name"].get<std::string>());
    return names;
}

std::mutex& packageOperations()
{
    static std::mutex mutex;
    return mutex;
}

} // namespace shell_calls
