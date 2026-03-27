#include "connection_file.h"
#include "../config.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <signal.h>

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

bool ConnectionFile::isPidAlive(int64_t pid)
{
    if (pid <= 0)
        return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
}

bool ConnectionFile::write(const std::string& instanceId, const std::string& token,
                           int64_t pid, const std::vector<std::string>& modulesDirs)
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

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs)
        return false;

    ofs << obj.dump(4) << "\n";
    return ofs.good();
}

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

    // Validate: check if PID is alive
    if (info.pid > 0 && isPidAlive(info.pid))
        info.valid = true;

    return info;
}

bool ConnectionFile::remove()
{
    std::error_code ec;
    return fs::remove(filePath(), ec);
}

bool ConnectionFile::isStale()
{
    ConnectionInfo info = read();
    if (info.pid <= 0)
        return true;
    return !isPidAlive(info.pid);
}
