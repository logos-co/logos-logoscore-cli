#include "issue_token_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>

namespace {

// Resolve a `--expires` value to an ISO 8601 UTC absolute deadline.
// Accepted forms:
//   - empty string (returned as-is) → non-expiring
//   - "<N>s|m|h|d" relative duration → now + N seconds/minutes/hours/days
//   - ISO 8601 absolute date in UTC ("2026-12-31T23:59:59Z" or
//     "2026-12-31") → returned as-is, normalized to "T00:00:00Z" if
//     date-only.
//
// Returns std::nullopt on parse failure so the caller can print a
// clear error rather than persisting garbage.
std::optional<std::string> resolveExpires(const std::string& s)
{
    if (s.empty()) return std::string{};

    static const std::regex relRe(R"(^(\d+)([smhd])$)");
    std::smatch m;
    if (std::regex_match(s, m, relRe)) {
        long n = 0;
        try {
            n = std::stol(m[1].str());
        } catch (const std::exception&) {
            return std::nullopt;
        }
        const char suffix = m[2].str()[0];
        long seconds = 0;
        auto checkedMul = [](long a, long b, long& out) {
            if (a < 0 || b <= 0) return false;
            if (a > std::numeric_limits<long>::max() / b) return false;
            out = a * b;
            return true;
        };
        switch (suffix) {
        case 's': seconds = n; break;
        case 'm': if (!checkedMul(n, 60, seconds))    return std::nullopt; break;
        case 'h': if (!checkedMul(n, 3600, seconds))  return std::nullopt; break;
        case 'd': if (!checkedMul(n, 86400, seconds)) return std::nullopt; break;
        default:  return std::nullopt;
        }
        const auto now = std::chrono::system_clock::now()
                       + std::chrono::seconds(seconds);
        const time_t tt = std::chrono::system_clock::to_time_t(now);
        struct tm utc{};
        gmtime_r(&tt, &utc);
        std::ostringstream ss;
        ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
        return ss.str();
    }

    static const std::regex dateOnlyRe(R"(^\d{4}-\d{2}-\d{2}$)");
    static const std::regex fullRe(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$)");
    if (std::regex_match(s, dateOnlyRe))
        return s + "T00:00:00Z";
    if (std::regex_match(s, fullRe))
        return s;
    return std::nullopt;
}

} // namespace

int IssueTokenCommand::execute(const std::vector<std::string>& args)
{
    CLI::App cli{"issue-token"};
    cli.set_help_flag();
    std::string name;
    std::string expires;
    bool replace   = false;
    bool localOnly = false;
    cli.add_option("--name", name, "Name for this client token")->required();
    cli.add_option("--expires", expires,
                   "Expiry: <N>s/m/h/d relative, or ISO date/timestamp UTC. "
                   "Default: non-expiring.");
    cli.add_flag("--replace", replace, "Replace an existing token with this name");
    cli.add_flag("--local-only", localOnly,
                 "Reject this token over non-LocalSocket transports");

    try {
        std::vector<std::string> argsCopy(args.rbegin(), args.rend());
        cli.parse(argsCopy);
    } catch (const CLI::ParseError&) {
        output().printError("INVALID_ARGS",
            "Usage: logoscore issue-token --name NAME [--expires DUR] [--replace] [--local-only]");
        return 1;
    }

    auto resolvedExpiry = resolveExpires(expires);
    if (!resolvedExpiry) {
        output().printError("INVALID_EXPIRES",
            fmt::format("Could not parse --expires '{}'. "
                        "Use Ns/Nm/Nh/Nd or ISO 8601 UTC.", expires));
        return 1;
    }

    TokenStore store;
    const auto outcome = store.issueToken(name, *resolvedExpiry, localOnly, replace);
    switch (outcome.status) {
        case TokenStore::IssueStatus::Ok:
            break;
        case TokenStore::IssueStatus::InvalidName:
            output().printError("INVALID_NAME",
                fmt::format("Token name '{}' is invalid (must be 1-64 chars, "
                            "alnum/dot/dash/underscore, no traversal).", name));
            return 1;
        case TokenStore::IssueStatus::AlreadyExists:
            output().printError("TOKEN_EXISTS",
                fmt::format("Token '{}' already exists. Use --replace to overwrite.", name));
            return 3;
        case TokenStore::IssueStatus::IoError:
            output().printError("IO_ERROR",
                fmt::format("Failed to persist token '{}' to disk. Check "
                            "permissions/free space under {}.",
                            name, Config::daemonTokensDir()));
            return 1;
    }

    std::string rawPath = store.rawTokenFilePath(name);

    LogosMap result;
    result["status"]     = "ok";
    result["name"]       = name;
    result["token"]      = outcome.token;
    result["file"]       = rawPath;
    result["local_only"] = localOnly;
    if (!resolvedExpiry->empty())
        result["expires_at"] = *resolvedExpiry;

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(fmt::format("Issued token for '{}'", name));
        output().printRaw(fmt::format("  file:  {}", rawPath));
        output().printRaw(fmt::format("  token: {}", outcome.token));
        if (!resolvedExpiry->empty())
            output().printRaw(fmt::format("  expires_at: {}", *resolvedExpiry));
        if (localOnly)
            output().printRaw("  local_only: true");
    }
    return 0;
}
