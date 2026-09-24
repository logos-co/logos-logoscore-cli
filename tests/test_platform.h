#ifndef LOGOSCTL_TEST_PLATFORM_H
#define LOGOSCTL_TEST_PLATFORM_H

// What the unit tests need from the OS, the same way on Unix and Windows.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace logosctl_test {

inline void setEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
    // The CRT's copy for getenv, the process block for children.
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

inline void setEnv(const char* name, const char* value) { setEnv(name, std::string(value)); }
inline void setEnv(const char* name, const std::filesystem::path& value) { setEnv(name, value.string()); }

inline void unsetEnv(const char* name)
{
#ifdef _WIN32
    _putenv_s(name, "");
    ::SetEnvironmentVariableA(name, nullptr);
#else
    ::unsetenv(name);
#endif
}

inline long long currentPid()
{
#ifdef _WIN32
    return static_cast<long long>(::GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

// The variable Config roots the default config dir at: $HOME on Unix,
// %LOCALAPPDATA% on Windows, which has no $HOME.
inline const char* homeVar()
{
#ifdef _WIN32
    return "LOCALAPPDATA";
#else
    return "HOME";
#endif
}

// Separator between the directories in PATH.
constexpr char kPathListSep =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

#ifdef _WIN32
// The child the Windows tests start (test_child.cpp), built beside this binary.
inline std::filesystem::path childPath()
{
    std::wstring self(MAX_PATH, L'\0');
    self.resize(::GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    return std::filesystem::path(self).parent_path() / "logosctl_test_child.exe";
}
#endif

} // namespace logosctl_test

#endif // LOGOSCTL_TEST_PLATFORM_H
