#include "issue_token_command.h"

#include "../../config.h"
#include "../../daemon/token_store.h"

#include <CLI/CLI.hpp>

#include <QJsonObject>
#include <QString>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
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
// clear error rather than persisting garbage. Empty input returns an
// empty string (the "no expiry" sentinel that DaemonStateFile encodes
// as JSON `null`).
std::optional<std::string> resolveExpires(const std::string& s)
{
    if (s.empty()) return std::string{};

    // Relative duration: integer + suffix.
    static const std::regex relRe(R"(^(\d+)([smhd])$)");
    std::smatch m;
    if (std::regex_match(s, m, relRe)) {
        const long n = std::stol(m[1].str());
        const char suffix = m[2].str()[0];
        long seconds = 0;
        switch (suffix) {
        case 's': seconds = n; break;
        case 'm': seconds = n * 60; break;
        case 'h': seconds = n * 3600; break;
        case 'd': seconds = n * 86400; break;
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

    // Absolute ISO 8601 (date-only or full timestamp).
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
    bool replace = false;
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
            QString("Could not parse --expires '%1'. Use Ns/Nm/Nh/Nd or ISO 8601 UTC.")
                .arg(QString::fromStdString(expires)));
        return 1;
    }

    TokenStore store(Config::configDir().toStdString());
    auto maybeToken = store.issueToken(name, *resolvedExpiry, localOnly, replace);
    if (!maybeToken) {
        output().printError("TOKEN_EXISTS",
            QString("Token '%1' already exists. Use --replace to overwrite.")
                .arg(QString::fromStdString(name)));
        return 3;
    }

    const QString rawPath = QString::fromStdString(store.rawTokenFilePath(name));

    QJsonObject result;
    result["status"]    = "ok";
    result["name"]      = QString::fromStdString(name);
    result["token"]     = QString::fromStdString(*maybeToken);
    result["file"]      = rawPath;
    result["local_only"] = localOnly;
    if (!resolvedExpiry->empty())
        result["expires_at"] = QString::fromStdString(*resolvedExpiry);

    if (output().isJsonMode()) {
        output().printSuccess(result);
    } else {
        output().printRaw(QString("Issued token for '%1'").arg(QString::fromStdString(name)));
        output().printRaw(QString("  file:  %1").arg(rawPath));
        output().printRaw(QString("  token: %1").arg(QString::fromStdString(*maybeToken)));
        if (!resolvedExpiry->empty())
            output().printRaw(QString("  expires_at: %1").arg(QString::fromStdString(*resolvedExpiry)));
        if (localOnly)
            output().printRaw(QString("  local_only: true"));
    }
    return 0;
}
