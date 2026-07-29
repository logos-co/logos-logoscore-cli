#include "yaml_json.h"

#include <yaml-cpp/yaml.h>

#include <sstream>

namespace yaml_json {
namespace {

// YAML scalars are untyped text; the tag tells us whether the author wrote
// `true`, `8645` or `"8645"`. yaml-cpp records that distinction, so we can
// recover real JSON types instead of turning every value into a string —
// which would silently break `port: 8645` and `verifyPeer: true`.
nlohmann::json scalarToJson(const YAML::Node& node)
{
    const std::string& tag = node.Tag();
    // An explicitly quoted scalar is a string by construction, whatever it
    // looks like. This is what keeps a version like "1.10" from becoming the
    // number 1.1.
    if (tag == "!") return node.Scalar();

    bool b = false;
    if (YAML::convert<bool>::decode(node, b)) return b;
    int64_t i = 0;
    if (YAML::convert<int64_t>::decode(node, i)) return i;
    double d = 0;
    if (YAML::convert<double>::decode(node, d)) return d;

    const std::string s = node.Scalar();
    // `key:` with nothing after it is null, not the empty string.
    if (s.empty() || s == "~" || s == "null") return nullptr;
    return s;
}

nlohmann::json nodeToJson(const YAML::Node& node)
{
    switch (node.Type()) {
    case YAML::NodeType::Null:
    case YAML::NodeType::Undefined:
        return nullptr;
    case YAML::NodeType::Scalar:
        return scalarToJson(node);
    case YAML::NodeType::Sequence: {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& child : node) arr.push_back(nodeToJson(child));
        return arr;
    }
    case YAML::NodeType::Map: {
        nlohmann::json obj = nlohmann::json::object();
        for (const auto& kv : node)
            obj[kv.first.as<std::string>()] = nodeToJson(kv.second);
        return obj;
    }
    }
    return nullptr;
}

void emit(YAML::Emitter& out, const nlohmann::json& v)
{
    switch (v.type()) {
    case nlohmann::json::value_t::object:
        out << YAML::BeginMap;
        for (auto it = v.begin(); it != v.end(); ++it) {
            out << YAML::Key << it.key() << YAML::Value;
            emit(out, it.value());
        }
        out << YAML::EndMap;
        break;
    case nlohmann::json::value_t::array:
        out << YAML::BeginSeq;
        for (const auto& e : v) emit(out, e);
        out << YAML::EndSeq;
        break;
    case nlohmann::json::value_t::string:
        out << v.get<std::string>();
        break;
    case nlohmann::json::value_t::boolean:
        out << v.get<bool>();
        break;
    case nlohmann::json::value_t::number_integer:
        out << v.get<int64_t>();
        break;
    case nlohmann::json::value_t::number_unsigned:
        out << v.get<uint64_t>();
        break;
    case nlohmann::json::value_t::number_float:
        out << v.get<double>();
        break;
    case nlohmann::json::value_t::null:
    default:
        out << YAML::Null;
        break;
    }
}

} // namespace

std::optional<nlohmann::json> parse(const std::string& text, std::string* errorOut)
{
    try {
        YAML::Node root = YAML::Load(text);
        // An empty document is a valid YAML file that says nothing; treat it
        // as an empty mapping so callers get defaults rather than an error.
        if (!root || root.IsNull()) return nlohmann::json::object();
        return nodeToJson(root);
    } catch (const std::exception& e) {
        if (errorOut) *errorOut = e.what();
        return std::nullopt;
    }
}

std::string dump(const nlohmann::json& value)
{
    YAML::Emitter out;
    emit(out, value);
    std::string s = out.c_str();
    if (!s.empty() && s.back() != '\n') s.push_back('\n');
    return s;
}

} // namespace yaml_json
