#ifndef LOGOSCORE_JSON_SCHEMA_H
#define LOGOSCORE_JSON_SCHEMA_H

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Type-checked reads for the configuration documents a human writes.
//
// nlohmann's `json::value(key, default)` THROWS `json::type_error` when the key
// is present carrying a different type than the default, and an uncaught throw
// terminates the process. `modules_dirs: /single/path` -- a scalar where a list
// belongs -- is an ordinary typo, so aborting on it is never the right answer.
//
// Every config read goes through `Reader` instead: a mismatch is recorded as a
// message naming the offending key and what was expected, the read yields the
// caller's default, and the caller turns the first recorded message into a
// normal error with nothing applied. Same shape as the unknown-key error
// `daemon config set` already produces -- name the key, say what was expected.
namespace json_schema {

// What a value actually is, phrased for the "but got ..." half of a message.
inline const char* typeName(const nlohmann::json& v)
{
    using vt = nlohmann::json::value_t;
    switch (v.type()) {
    case vt::null:            return "nothing";
    case vt::object:          return "a mapping";
    case vt::array:           return "a list";
    case vt::string:          return "a string";
    case vt::boolean:         return "true or false";
    case vt::number_integer:
    case vt::number_unsigned: return "a whole number";
    case vt::number_float:    return "a decimal number";
    default:                  return "an unsupported value";
    }
}

// First-error-wins collector. Once a document is known to be wrong it is not
// applied at all, and later complaints are usually consequences of the first
// one -- a single precise message is more actionable than a list.
class Errors {
public:
    bool ok() const { return m_message.empty(); }
    const std::string& message() const { return m_message; }

    void note(std::string message)
    {
        if (m_message.empty()) m_message = std::move(message);
    }

    void mismatch(const std::string& key, const std::string& expected,
                  const nlohmann::json& got)
    {
        note(key + ": expected " + expected + ", but got " + typeName(got) + ".");
    }

private:
    std::string m_message;
};

// Reads one mapping. `prefix` is the dotted path of that mapping inside the
// document ("" at the top level, "logging." one level down), so every message
// names the key the way the operator wrote it.
class Reader {
public:
    Reader(const nlohmann::json& obj, Errors& errors, std::string prefix = {})
        : m_obj(obj), m_errors(errors), m_prefix(std::move(prefix)) {}

    // Full dotted path of `key`, for callers that report their own errors.
    std::string path(const std::string& key) const { return m_prefix + key; }

    std::string str(const std::string& key, std::string dflt = {}) const
    {
        const nlohmann::json* v = present(key);
        if (!v) return dflt;
        if (!v->is_string()) {
            m_errors.mismatch(path(key), "a string", *v);
            return dflt;
        }
        return v->get<std::string>();
    }

    bool boolean(const std::string& key, bool dflt) const
    {
        const nlohmann::json* v = present(key);
        if (!v) return dflt;
        if (!v->is_boolean()) {
            m_errors.mismatch(path(key), "true or false", *v);
            return dflt;
        }
        return v->get<bool>();
    }

    // One accessor for every integral field: YAML has a single integer type, so
    // callers differ only in the range they accept. An out-of-range value is
    // reported the same way as a wrong type -- it is the same class of mistake.
    int64_t integer(const std::string& key, int64_t dflt,
                    int64_t min = std::numeric_limits<int64_t>::min(),
                    int64_t max = std::numeric_limits<int64_t>::max()) const
    {
        const nlohmann::json* v = present(key);
        if (!v) return dflt;
        if (!v->is_number_integer()) {
            m_errors.mismatch(path(key), "a whole number", *v);
            return dflt;
        }
        // An unsigned value past INT64_MAX would wrap on get<int64_t>(), so
        // range-report it before converting.
        const bool tooBig =
            v->is_number_unsigned() &&
            v->get<uint64_t>() >
                static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        const int64_t n = tooBig ? 0 : v->get<int64_t>();
        if (tooBig || n < min || n > max) {
            m_errors.note(path(key) + ": expected a whole number between " +
                          std::to_string(min) + " and " + std::to_string(max) +
                          (tooBig ? std::string(".")
                                  : ", but got " + std::to_string(n) + "."));
            return dflt;
        }
        return n;
    }

    std::vector<std::string> stringList(const std::string& key,
                                        std::vector<std::string> dflt = {}) const
    {
        const nlohmann::json* v = present(key);
        if (!v) return dflt;
        if (!v->is_array()) {
            m_errors.mismatch(path(key), "a list of strings", *v);
            return dflt;
        }
        std::vector<std::string> out;
        out.reserve(v->size());
        for (std::size_t i = 0; i < v->size(); ++i) {
            const nlohmann::json& e = (*v)[i];
            if (!e.is_string()) {
                m_errors.mismatch(path(key) + "[" + std::to_string(i) + "]",
                                  "a string", e);
                return dflt;
            }
            out.push_back(e.get<std::string>());
        }
        return out;
    }

    // Nested mapping / list. nullptr when the key is absent (the caller keeps
    // its defaults); nullptr *and* a recorded error when it is present with the
    // wrong type -- silently ignoring it would drop the operator's intent with
    // nothing to explain it.
    const nlohmann::json* mapping(const std::string& key,
                                  const std::string& expected = "a mapping") const
    {
        const nlohmann::json* v = present(key);
        if (!v) return nullptr;
        if (!v->is_object()) {
            m_errors.mismatch(path(key), expected, *v);
            return nullptr;
        }
        return v;
    }

    const nlohmann::json* list(const std::string& key,
                               const std::string& expected = "a list") const
    {
        const nlohmann::json* v = present(key);
        if (!v) return nullptr;
        if (!v->is_array()) {
            m_errors.mismatch(path(key), expected, *v);
            return nullptr;
        }
        return v;
    }

    Errors& errors() const { return m_errors; }

private:
    // A key that is absent -- or explicitly empty (`key:` with nothing after
    // it, which YAML reads as null) -- means "not set", and the caller's
    // default wins. `find` on a non-mapping yields end(), so a Reader built
    // over the wrong kind of value degrades to all-defaults rather than
    // throwing.
    const nlohmann::json* present(const std::string& key) const
    {
        auto it = m_obj.find(key);
        if (it == m_obj.end() || it->is_null()) return nullptr;
        return &*it;
    }

    const nlohmann::json& m_obj;
    Errors&               m_errors;
    std::string           m_prefix;
};

} // namespace json_schema

#endif // LOGOSCORE_JSON_SCHEMA_H
