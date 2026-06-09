#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace strutil {

inline bool starts_with(const std::string& s, char c)
{
    return !s.empty() && s.front() == c;
}

inline bool starts_with(const std::string& s, const std::string& prefix)
{
    return s.size() >= prefix.size()
        && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool ends_with(const std::string& s, char c)
{
    return !s.empty() && s.back() == c;
}

inline bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline std::string join(const std::vector<std::string>& v, const std::string& sep)
{
    std::string result;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) result += sep;
        result += v[i];
    }
    return result;
}

inline std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// Strip leading/trailing ASCII whitespace. Returns "" for all-whitespace input.
inline std::string trim(const std::string& s)
{
    const char* ws = " \t\n\r\f\v";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos)
        return "";
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

} // namespace strutil

#endif // STRING_UTILS_H
