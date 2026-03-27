#include "paths.h"

#include <filesystem>
#include <climits>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace paths {

std::string executableDir()
{
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0) {
        return fs::path(buf.c_str()).parent_path().string();
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return fs::path(buf).parent_path().string();
    }
#endif
    return {};
}

std::string bundledModulesDir()
{
    std::string binDir = executableDir();
    if (binDir.empty())
        return {};

    fs::path candidate = fs::path(binDir) / ".." / "modules";
    std::error_code ec;
    auto resolved = fs::canonical(candidate, ec);
    if (!ec && fs::is_directory(resolved, ec))
        return resolved.string();

    return {};
}

} // namespace paths
