#ifndef LOGOSCTL_TEST_PLATFORM_H
#define LOGOSCTL_TEST_PLATFORM_H

// What the unit tests need from the OS, the same way on Unix and Windows.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwchar>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
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

// Words of a command line with no quoting in it, as the CLI tests write them.
inline std::vector<std::string> splitArgs(const std::string& line)
{
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string word; in >> word;) words.push_back(word);
    return words;
}

#ifdef _WIN32
// The tests' UTF-8 text as UTF-16, for CreateProcessW.
inline std::wstring widen(const std::string& text)
{
    if (text.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// Quoted so CommandLineToArgvW gives the child `arg` back unchanged.
inline std::wstring quoteArg(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        out.append(*it == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}
#endif

// Held while a child starts: the handles it inherits exist only inside it, so
// no other thread's child can inherit them and hold a pipe open.
inline std::mutex& spawnMutex()
{
    static std::mutex mutex;
    return mutex;
}

#ifdef _WIN32
// This process's environment with `env` applied, as CreateProcessW takes it.
// An empty value unsets the variable, as _putenv_s does.
inline std::wstring environmentBlock(const std::vector<std::pair<std::string, std::string>>& env)
{
    std::vector<std::wstring> vars;
    if (wchar_t* block = ::GetEnvironmentStringsW()) {
        for (const wchar_t* var = block; *var; var += std::wcslen(var) + 1) vars.emplace_back(var);
        ::FreeEnvironmentStringsW(block);
    }
    for (const auto& [name, value] : env) {
        const std::wstring key = widen(name) + L"=";
        vars.erase(std::remove_if(vars.begin(), vars.end(), [&key](const std::wstring& var) {
            return ::_wcsnicmp(var.c_str(), key.c_str(), key.size()) == 0;
        }), vars.end());
        if (!value.empty()) vars.push_back(key + widen(value));
    }
    std::wstring block;
    for (const auto& var : vars) block.append(var).push_back(L'\0');
    block.push_back(L'\0');
    return block;
}
#else
// This process's environment with `env` applied, as posix_spawn takes it.
inline std::vector<std::string> environmentList(
    const std::vector<std::pair<std::string, std::string>>& env)
{
    std::vector<std::string> vars;
    for (char** var = environ; *var; ++var) vars.emplace_back(*var);
    for (const auto& [name, value] : env) {
        const std::string key = name + "=";
        vars.erase(std::remove_if(vars.begin(), vars.end(), [&key](const std::string& var) {
            return var.compare(0, key.size(), key) == 0;
        }), vars.end());
        vars.push_back(key + value);
    }
    return vars;
}
#endif

// Runs `exe args...` with stdout and stderr captured together into `output`,
// as `timeout N exe args 2>&1` does: killed after `timeoutSecs` (none if 0),
// reported as exit 124. `env` sets variables for the child only. Safe to call
// from several threads at once.
inline int runProcess(const std::filesystem::path& exe, const std::vector<std::string>& args,
                      std::string* output, int timeoutSecs,
                      const std::vector<std::pair<std::string, std::string>>& env = {})
{
    std::string sink;
    if (!output) output = &sink;
    const bool bounded = timeoutSecs > 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSecs);
#ifdef _WIN32
    std::wstring environment = environmentBlock(env);
    std::wstring command = quoteArg(exe.wstring());
    for (const auto& arg : args) command += L" " + quoteArg(widen(arg));
    HANDLE readEnd = nullptr;
    PROCESS_INFORMATION info{};
    {
        std::lock_guard<std::mutex> lock(spawnMutex());
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE writeEnd = nullptr;
        if (!::CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return -1;
        ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &inherit, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nul;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        const BOOL started = ::CreateProcessW(exe.wstring().c_str(), command.data(), nullptr,
                                              nullptr, TRUE,
                                              CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                              environment.data(), nullptr, &startup, &info);
        ::CloseHandle(writeEnd);
        if (nul != INVALID_HANDLE_VALUE) ::CloseHandle(nul);
        if (!started) { ::CloseHandle(readEnd); return -1; }
    }
    ::CloseHandle(info.hThread);
    // Drained on a thread of its own, so a full pipe never stalls the child.
    std::thread reader([readEnd, output]() {
        char buffer[4096];
        DWORD n = 0;
        while (::ReadFile(readEnd, buffer, sizeof(buffer), &n, nullptr) && n > 0)
            output->append(buffer, n);
    });
    DWORD wait = INFINITE;
    if (bounded) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        wait = static_cast<DWORD>(std::max<long long>(0, left.count()));
    }
    int code = 124;
    if (::WaitForSingleObject(info.hProcess, wait) == WAIT_OBJECT_0) {
        DWORD exitCode = 0;
        ::GetExitCodeProcess(info.hProcess, &exitCode);
        code = static_cast<int>(exitCode);
    } else {
        ::TerminateProcess(info.hProcess, 124);
        ::WaitForSingleObject(info.hProcess, INFINITE);
    }
    ::CloseHandle(info.hProcess);
    reader.join();   // ends when every holder of the write end has exited
    ::CloseHandle(readEnd);
    return code;
#else
    std::vector<std::string> envStore = environmentList(env);
    std::vector<char*> envp;
    for (auto& var : envStore) envp.push_back(var.data());
    envp.push_back(nullptr);
    std::vector<std::string> argvStore{exe.string()};
    argvStore.insert(argvStore.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& arg : argvStore) argv.push_back(arg.data());
    argv.push_back(nullptr);
    int fds[2];
    pid_t pid = 0;
    int rc = 0;
    {
        std::lock_guard<std::mutex> lock(spawnMutex());
        if (::pipe(fds) != 0) return -1;
        // The child's stdout and stderr are dup2 copies; the pipe itself
        // closes on exec in every child.
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
        rc = ::posix_spawn(&pid, argvStore[0].c_str(), &actions, nullptr, argv.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);
    }
    if (rc != 0) { ::close(fds[0]); return -1; }
    bool timedOut = false;
    for (;;) {
        int wait = -1;
        if (bounded) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) { timedOut = true; break; }
            wait = static_cast<int>(left);
        }
        pollfd pfd{fds[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, wait);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) continue;
        char buffer[4096];
        const ssize_t n = ::read(fds[0], buffer, sizeof(buffer));
        if (n > 0) { output->append(buffer, static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;   // EOF: every holder of the write end is gone
    }
    if (timedOut) ::kill(pid, SIGTERM);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    ::close(fds[0]);
    if (timedOut) return 124;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
#endif
}

} // namespace logosctl_test

#endif // LOGOSCTL_TEST_PLATFORM_H
