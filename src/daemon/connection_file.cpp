#include "connection_file.h"
#include "../config.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string currentUtcIso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    struct tm utc{};
    gmtime_r(&tt, &utc);
    std::ostringstream ss;
    ss << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::string ConnectionFile::filePath()
{
    return Config::connectionFilePath().toStdString();
}

// ── Write ──────────────────────────────────────────────────────────────────

bool ConnectionFile::write(const std::string& instanceId, const std::string& token,
                           int64_t pid, const std::vector<std::string>& modulesDirs,
                           const std::vector<TransportInfo>& transports)
{
    fs::path path(filePath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
        return false;

    json obj;
    obj["instance_id"] = instanceId;
    obj["token"] = token;
    obj["pid"] = pid;
    obj["started_at"] = currentUtcIso8601();
    obj["modules_dirs"] = modulesDirs;

    if (!transports.empty()) {
        json arr = json::array();
        for (const auto& t : transports) {
            json j;
            j["protocol"] = t.protocol;
            if (t.protocol != "local") {
                j["host"] = t.host;
                j["port"] = t.port;
                // Codec only has meaning for plain transports; local uses
                // QRemoteObjects' own format and ignores it.
                j["codec"] = t.codec.empty() ? std::string("json") : t.codec;
            }
            if (t.protocol == "tcp_ssl") {
                if (!t.caFile.empty()) j["ca_file"] = t.caFile;
                j["verify_peer"] = t.verifyPeer;
            }
            arr.push_back(std::move(j));
        }
        obj["transports"] = std::move(arr);
    }

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs)
        return false;

    ofs << obj.dump(4) << "\n";
    return ofs.good();
}

// ── Read ── (pure parse; no liveness check) ────────────────────────────────

ConnectionInfo ConnectionFile::read()
{
    ConnectionInfo info;

    std::ifstream ifs(filePath());
    if (!ifs)
        return info;

    json obj;
    try {
        obj = json::parse(ifs);
    } catch (...) {
        return info;
    }

    info.instanceId  = obj.value("instance_id", "");
    info.token       = obj.value("token", "");
    info.pid         = obj.value("pid", int64_t(-1));
    info.startedAt   = obj.value("started_at", "");
    info.modulesDirs = obj.value("modules_dirs", std::vector<std::string>{});

    if (obj.contains("transports") && obj["transports"].is_array()) {
        for (const auto& j : obj["transports"]) {
            if (!j.is_object()) continue;
            TransportInfo t;
            t.protocol   = j.value("protocol", std::string{});
            t.host       = j.value("host", std::string{});
            // Validate the port through a wide int before narrowing —
            // a JSON value of e.g. 70000 cast straight to uint16_t would
            // silently wrap to 4464 and we'd then dial an unrelated port.
            const int rawPort = j.value("port", 0);
            if (rawPort < 0 || rawPort > 0xFFFF) continue;
            t.port       = static_cast<uint16_t>(rawPort);
            t.caFile     = j.value("ca_file", std::string{});
            t.verifyPeer = j.value("verify_peer", true);
            t.codec      = j.value("codec", std::string{"json"});
            if (!t.protocol.empty()) info.transports.push_back(std::move(t));
        }
    }

    info.fileOk = !info.instanceId.empty();
    return info;
}

bool ConnectionFile::remove()
{
    std::error_code ec;
    return fs::remove(filePath(), ec);
}
