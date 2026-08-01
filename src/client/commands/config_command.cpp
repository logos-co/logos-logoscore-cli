#include "config_command.h"
#include "../../config.h"
#include "../../yaml_json.h"
#include "../../daemon/daemon_state.h"
#include "../client_state.h"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace {

// `FILE` accepts a path, `@path` (the same convention `call` uses for file
// arguments), or `-` for stdin. Returns nullopt with a diagnostic already
// printed to `err` on failure.
std::optional<std::string> readDocument(const std::string& spec, std::string* err)
{
    if (spec == "-") {
        std::stringstream buf;
        buf << std::cin.rdbuf();
        return buf.str();
    }
    const std::string path = (!spec.empty() && spec[0] == '@') ? spec.substr(1) : spec;
    std::ifstream ifs(path);
    if (!ifs) {
        if (err) *err = "Could not open '" + path + "'.";
        return std::nullopt;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();
    return buf.str();
}

} // namespace

int ConfigCommand::execute(const std::vector<std::string>& args)
{
    const std::string group = m_daemonSide ? "daemon" : "client";
    if (args.empty()) {
        output().printError("INVALID_ARGS",
            "Usage: logosctl " + group + " config <show|set FILE>");
        return 1;
    }
    if (args[0] == "show") return show();
    if (args[0] == "set") {
        if (args.size() < 2) {
            output().printError("INVALID_ARGS",
                "Usage: logosctl " + group + " config set <FILE|@FILE|->");
            return 1;
        }
        return set({args.begin() + 1, args.end()});
    }
    output().printError("INVALID_ARGS", "Unknown config subcommand: " + args[0]);
    return 1;
}

int ConfigCommand::set(const std::vector<std::string>& args)
{
    std::string readErr;
    auto text = readDocument(args[0], &readErr);
    if (!text) {
        output().printError("IO_ERROR", readErr);
        return 1;
    }

    // Parse and validate BEFORE touching the session, so a malformed document
    // leaves the existing config intact rather than half-replacing it.
    std::string parseErr;
    auto parsed = yaml_json::parse(*text, &parseErr);
    if (!parsed) {
        output().printError("INVALID_CONFIG", "Not valid YAML: " + parseErr);
        return 1;
    }
    if (!parsed->is_object()) {
        output().printError("INVALID_CONFIG",
            "The config document must be a mapping at the top level.");
        return 1;
    }

    // Reject keys we don't understand. The loaders below ignore anything
    // unrecognised, so without this a near-miss like `insecureTcp` (the key is
    // `insecure_tcp`) would be accepted, silently dropped, and the daemon
    // would boot with the operator's intent missing and nothing to explain it.
    {
        static const std::set<std::string> kDaemonKeys{
            "version", "modules", "modules_dirs", "persistence_path", "ssl",
            "insecure_tcp", "access_policy", "access_group", "dirs", "logging",
            "signature_policy",
        };
        static const std::set<std::string> kClientKeys{
            "version", "daemon", "token_file", "instance_id",
        };
        const auto& known = m_daemonSide ? kDaemonKeys : kClientKeys;
        std::vector<std::string> unknown;
        for (auto it = parsed->begin(); it != parsed->end(); ++it)
            if (!known.count(it.key())) unknown.push_back(it.key());
        if (!unknown.empty()) {
            output().printError("INVALID_CONFIG",
                fmt::format("Unknown key(s): {}. Known keys are: {}.",
                            fmt::join(unknown, ", "), fmt::join(known, ", ")));
            return 1;
        }
    }

    // Stamp the schema version so a hand-written document doesn't have to
    // carry it, and round-trip through the emitter so what lands on disk is
    // canonical rather than whatever formatting the author used.
    nlohmann::json doc = std::move(*parsed);
    doc["version"] = m_daemonSide ? kDaemonConfigSchemaVersion
                                  : kClientStateSchemaVersion;
    const std::string canonical = yaml_json::dump(doc);

    // Validate the exact bytes we are about to write, through the same loader
    // the daemon and client use at startup — and do it BEFORE writing.
    //
    // This used to run after the file was on disk, so a document that parsed
    // as YAML but failed the schema was written, *then* reported as an error:
    // the command exited 1 while the session was left holding a config the
    // daemon would refuse to boot from. Re-parsing the canonical text here
    // (rather than validating `doc` directly) keeps the earlier guarantee that
    // what was checked is what lands on disk, with nothing written on failure.
    {
        std::string err;
        auto reparsed = yaml_json::parse(canonical, &err);
        if (!reparsed) {
            output().printError("INVALID_CONFIG", "Not valid YAML: " + err);
            return 1;
        }
        const bool valid = m_daemonSide
            ? parseDaemonConfigDocument(*reparsed, &err).has_value()
            : parseClientStateDocument(*reparsed, &err).has_value();
        if (!valid) {
            output().printError("INVALID_CONFIG", err);
            return 1;
        }
    }

    const std::string path = m_daemonSide ? Config::daemonConfigPath()
                                          : Config::clientConfigPath();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) {
        output().printError("IO_ERROR", "Could not create " + path);
        return 1;
    }

    // Write beside the target and rename into place. Opening the real path
    // with `trunc` would destroy the previous config before the new bytes were
    // safely down, so an I/O failure mid-write left the session holding a
    // truncated file — the same half-applied state the validation above exists
    // to prevent. The rename is the atomic publish step: a reader sees either
    // the old config or the new one.
    const fs::path tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream ofs(tmp, std::ios::trunc);
        if (!ofs) {
            output().printError("IO_ERROR", "Could not write " + path);
            return 1;
        }
        ofs << canonical;
        ofs.close();
        if (!ofs) {
            fs::remove(tmp, ec);
            output().printError("IO_ERROR", "Could not write " + path);
            return 1;
        }
    }
    // Keep whatever permissions the file being replaced had — a session shared
    // with an OS group would otherwise silently lose its group access.
    struct stat st;
    if (::stat(path.c_str(), &st) == 0)
        ::chmod(tmp.c_str(), st.st_mode & 07777);
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        output().printError("IO_ERROR", "Could not write " + path);
        return 1;
    }

    if (output().isJsonMode()) {
        output().printSuccess(LogosMap{{"status", "ok"}, {"path", path}});
    } else {
        output().printRaw(fmt::format("Wrote {}", path));
        if (m_daemonSide)
            output().printRaw("Restart the daemon for it to take effect.");
    }
    return 0;
}

int ConfigCommand::show()
{
    const std::string path = m_daemonSide ? Config::daemonConfigPath()
                                          : Config::clientConfigPath();
    std::ifstream ifs(path);
    if (!ifs) {
        // Absent is a normal state, not an error: a session with no config
        // runs on defaults.
        if (output().isJsonMode()) {
            output().printSuccess(LogosMap{{"path", path}, {"exists", false}});
        } else {
            output().printRaw(fmt::format("{}  (does not exist — using defaults)", path));
        }
        return 0;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();

    if (output().isJsonMode()) {
        auto parsed = yaml_json::parse(buf.str());
        output().printSuccess(LogosMap{
            {"path", path},
            {"exists", true},
            {"config", parsed ? *parsed : nlohmann::json(nullptr)},
        });
        return 0;
    }
    output().printRaw(fmt::format("# {}", path));
    output().printRaw(buf.str());
    return 0;
}
