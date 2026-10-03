#ifndef LOGOSCTL_UTF8_ARGS_H
#define LOGOSCTL_UTF8_ARGS_H

// main()'s arguments as UTF-8, which is what the CLI and JSON take. On Windows
// the CRT hands them over in the ANSI code page: "é" aborted the JSON encoder
// and "日本" arrived as "??".

#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <clocale>
#endif

namespace logosctl {

class Utf8Args {
public:
    Utf8Args(int& argc, char**& argv)
    {
#ifdef _WIN32
        int count = 0;
        if (LPWSTR* wide = ::CommandLineToArgvW(::GetCommandLineW(), &count)) {
            for (int i = 0; i < count; ++i) m_args.push_back(toUtf8(wide[i]));
            ::LocalFree(wide);
            for (auto& arg : m_args) m_argv.push_back(arg.data());
            m_argv.push_back(nullptr);
            argc = count;
            argv = m_argv.data();
        }
        // std::filesystem decodes narrow strings by LC_CTYPE, which is ASCII-only in the C locale.
        std::setlocale(LC_CTYPE, ".UTF-8");
#else
        (void)argc;
        (void)argv;
#endif
    }

    Utf8Args(const Utf8Args&) = delete;
    Utf8Args& operator=(const Utf8Args&) = delete;

private:
#ifdef _WIN32
    static std::string toUtf8(const wchar_t* wide)
    {
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1) return {};
        std::string out(static_cast<size_t>(size - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr, nullptr);
        return out;
    }
#endif

    std::vector<std::string> m_args;
    std::vector<char*> m_argv;
};

} // namespace logosctl

#endif // LOGOSCTL_UTF8_ARGS_H
