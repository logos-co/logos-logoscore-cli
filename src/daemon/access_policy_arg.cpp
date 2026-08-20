#include "daemon/access_policy_arg.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace logoscore {

std::optional<std::string> resolveAccessPolicyArg(const std::string& arg,
                                                  std::string* error)
{
    auto fail = [&](std::string why) -> std::optional<std::string> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };

    // Checked before the file branch, so `--access-policy enforce` never gets
    // read as a relative path named "enforce".
    if (arg == kEnforceAlias)
        return std::string(kEnforceEnvelope);

    std::string content;
    std::string source;  // for diagnostics

    auto firstNonSpace = std::find_if(arg.begin(), arg.end(),
        [](unsigned char c) { return !std::isspace(c); });
    const bool looksInline = (firstNonSpace != arg.end() && *firstNonSpace == '{');

    if (looksInline) {
        content = arg;
        source = "inline --access-policy JSON";
    } else {
        std::ifstream ifs(arg, std::ios::binary);
        if (!ifs)
            return fail("--access-policy file '" + arg + "' could not be opened.");
        std::ostringstream ss;
        ss << ifs.rdbuf();
        content = ss.str();
        source = "--access-policy file '" + arg + "'";
    }

    // Parse-check only; schema enforcement is the runtime's job.
    try {
        (void)nlohmann::json::parse(content);
    } catch (const std::exception& e) {
        return fail(source + " is not valid JSON: " + e.what());
    }

    return content;
}

} // namespace logoscore
