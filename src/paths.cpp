#include "paths.h"

#include <filesystem>
#include <climits>
#include <cstdlib>
#include <cstring>

#include <unistd.h>   // access() — present on mingw-w64 too

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
// Windows has no execute permission bit -- executability is decided by the
// file's contents, and `access(p, X_OK)` is not even declarable (there is no
// X_OK). Existence plus the is_regular_file() check callers already do is the
// meaningful test here.
constexpr int kExecAccessMode = 0;      // F_OK
constexpr char kPathListSep = ';';
#else
constexpr int kExecAccessMode = X_OK;
constexpr char kPathListSep = ':';
#endif
}  // namespace

namespace fs = std::filesystem;

namespace paths {

#ifdef _WIN32
// Shared by executablePath()/executableDir(): GetModuleFileNameW truncates
// rather than failing when the buffer is too small (and on older Windows does
// not even NUL-terminate), so grow until it fits.
static std::wstring moduleFileNameW()
{
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(),
                                             static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) { buf.resize(n); return buf; }
        if (buf.size() > 32768) return {};   // far past MAX_PATH; give up
        buf.resize(buf.size() * 2);
    }
}
#endif

std::string executablePath()
{
#ifdef _WIN32
    const std::wstring w = moduleFileNameW();
    return w.empty() ? std::string{} : fs::path(w).string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) == 0)
        return fs::path(buf.c_str()).string();
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return std::string(buf);
    }
#endif
    return {};
}

std::string executableDir()
{
#ifdef _WIN32
    const std::wstring w = moduleFileNameW();
    return w.empty() ? std::string{} : fs::path(w).parent_path().string();
#elif defined(__APPLE__)
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

namespace {
std::string siblingDir(const char* name)
{
    std::string binDir = executableDir();
    if (binDir.empty())
        return {};

    fs::path candidate = fs::path(binDir) / ".." / name;
    std::error_code ec;
    auto resolved = fs::canonical(candidate, ec);
    if (!ec && fs::is_directory(resolved, ec))
        return resolved.string();

    return {};
}
}  // namespace

std::string bundledPackageModulesDir() { return siblingDir("modules-pkg"); }

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



std::string relaunchPath(const char* argv0)
{
    auto usable = [](const fs::path& p) {
        std::error_code ec;
        // .string(), not .c_str(): fs::path::value_type is wchar_t on Windows,
        // so c_str() yields const wchar_t* and will not convert.
        return !p.empty() && fs::is_regular_file(p, ec)
            && ::access(p.string().c_str(), kExecAccessMode) == 0;
    };

    // Map a portable bundle's hidden ELF back to the launcher beside it.
    //
    //   bin/logosctl        the launcher: a shell script
    //   bin/.logosctl.elf   the real ELF, whose PT_INTERP names a loader that
    //                       does not exist on the host
    //
    // The launcher exists precisely because that ELF cannot be started on its
    // own -- it runs it through a known-good ld.so. And because it does, BOTH
    // argv[0] and /proc/self/exe name the ELF, not the launcher: ld.so drops
    // itself from argv, leaving the program to see the ELF as argv[0]. So
    // there is no source of truth to prefer here; the mapping has to be
    // applied to whatever we end up with. Exec'ing the ELF fails with ENOENT
    // -- the kernel reporting the missing interpreter, not the missing file --
    // and a detached daemon then dies with status 127, no output and no log.
    auto launcherFor = [&usable](const fs::path& p) {
        const std::string fn = p.filename().string();
        if (fn.size() > std::strlen(".") + std::strlen(".elf")
            && fn.front() == '.'
            && fn.compare(fn.size() - 4, 4, ".elf") == 0) {
            const fs::path sibling =
                p.parent_path() / fn.substr(1, fn.size() - 5);
            if (usable(sibling)) return sibling;
        }
        return p;
    };

    const std::string a0 = argv0 ? argv0 : "";
    if (!a0.empty()) {
        if (a0.find('/') != std::string::npos) {
            // Invoked by path. Absolutise now: the daemon we are about to
            // spawn may run from a different working directory, and a
            // relative argv[0] would then point at nothing.
            std::error_code ec;
            const fs::path abs = fs::absolute(a0, ec);
            if (!ec && usable(abs))
                return launcherFor(abs.lexically_normal()).string();
        } else {
            // Invoked by bare name: find the same thing the shell found.
            if (const char* pathEnv = std::getenv("PATH")) {
                std::string dirs(pathEnv);
                std::size_t start = 0;
                while (start <= dirs.size()) {
                    const std::size_t sep = dirs.find(kPathListSep, start);
                    const std::string dir = dirs.substr(
                        start, sep == std::string::npos ? std::string::npos
                                                        : sep - start);
                    if (!dir.empty() && usable(fs::path(dir) / a0))
                        return launcherFor(
                            (fs::path(dir) / a0).lexically_normal()).string();
                    if (sep == std::string::npos) break;
                    start = sep + 1;
                }
            }
        }
    }

    // Last resort: what the OS says we are, mapped through the same launcher
    // rule, since /proc/self/exe names the hidden ELF just as argv[0] does.
    return launcherFor(fs::path(executablePath())).string();
}

} // namespace paths
