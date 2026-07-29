#ifndef YAML_JSON_H
#define YAML_JSON_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

// YAML <-> JSON bridge.
//
// The split is by audience, not by convenience: files a human is expected to
// open and edit are YAML (daemon/config.yaml, client/config.yaml), while
// everything the daemon and the modules own stays JSON (state.json,
// tokens.json, auto.json, the downloader's repositories.json). YAML buys
// comments and readable nesting where an operator has to hand-write a
// transport block; it buys nothing for a file only a program reads.
//
// Converting through nlohmann::json rather than parsing YAML into the config
// structs directly means the existing, already-validated
// daemonConfigFromJson / clientStateFromJson converters keep doing the real
// schema work — the format change stops at the edge.
namespace yaml_json {

// Parse YAML text into JSON. Returns nullopt on malformed YAML, with the
// parser's message in `errorOut`.
//
// YAML is a superset of JSON, so a JSON document parses too.
std::optional<nlohmann::json> parse(const std::string& text,
                                    std::string* errorOut = nullptr);

// Serialize JSON as YAML. Objects and arrays become block-style mappings and
// sequences; scalars keep their JSON types (an unquoted 8645 stays a number,
// a quoted "0.1.0" stays a string).
std::string dump(const nlohmann::json& value);

} // namespace yaml_json

#endif // YAML_JSON_H
