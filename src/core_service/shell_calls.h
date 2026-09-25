#ifndef SHELL_CALLS_H
#define SHELL_CALLS_H

// The daemon's shell binding, "logoscore": its lifecycle calls go through
// liblogos' core_service, as every embedder's do.

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <vector>

struct logos_consumer;

namespace shell_calls {

// Once, after logos_core_start(); released before logos_core_cleanup().
void install(logos_consumer* binding);
void release();

// core_service's answer; null when the call failed or there is no binding.
nlohmann::json call(const char* method, const nlohmann::json& args, int timeoutMs);

// loadModule(name, "required_and_optional"): true when the module is up.
bool load(const std::string& name);
bool unload(const std::string& name, bool withDependents);
void refresh();
std::vector<std::string> loaded();

// Package operations run inside core_service's extension and call core_service
// again, so they take turns: several at once could fill its call slots and wait
// on themselves.
std::mutex& packageOperations();

} // namespace shell_calls

#endif
