#ifndef LOGOS_PLATFORM_COMPAT_H
#define LOGOS_PLATFORM_COMPAT_H

// The two POSIX calls that appear all over this CLI and have no drop-in
// Windows equivalent. Everything else that differs is handled where it is
// used, because the difference is structural (fork/exec, sockets, the signal
// self-pipe) rather than a rename.

#include <sys/types.h>   // mode_t — mingw-w64 defines it as unsigned short
#include <cstdio>
#include <cstdlib>
#include <ctime>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace logosctl {

// gmtime_r / localtime_r. The CRT spells these gmtime_s / localtime_s and --
// the trap -- takes THE SAME ARGUMENTS IN THE OPPOSITE ORDER, destination
// first. Both are pointers here so a mechanical rename would not compile, but
// the same reversal in the *_s functions that take (buf, size) silently does,
// which is why this is centralised rather than #ifdef'd at each call site.
inline std::tm* gmtimeR(const std::time_t* t, std::tm* out)
{
#ifdef _WIN32
    return ::gmtime_s(out, t) == 0 ? out : nullptr;
#else
    return ::gmtime_r(t, out);
#endif
}

inline std::tm* localtimeR(const std::time_t* t, std::tm* out)
{
#ifdef _WIN32
    return ::localtime_s(out, t) == 0 ? out : nullptr;
#else
    return ::localtime_r(t, out);
#endif
}

// timegm(3) — interpret a struct tm as UTC rather than local time. The CRT
// calls it _mkgmtime. (mktime is NOT a substitute: it applies the local
// timezone, so a token's expiry would shift by the machine's UTC offset.)
inline std::time_t timegmUtc(std::tm* tm)
{
#ifdef _WIN32
    return ::_mkgmtime(tm);
#else
    return ::timegm(tm);
#endif
}

// strptime(3) restricted to the one format this CLI ever parses: the
// "%Y-%m-%dT%H:%M:%SZ" UTC stamp it writes itself (token expiry).
//
// Unlike gmtime_r, strptime has no Windows counterpart at all -- reordered or
// otherwise -- so the Windows branch is a hand-rolled parse. It is deliberately
// STRICT and %n-anchored to the end of the string: the one caller treats a
// parse failure as "expired", so being lax here would let a malformed
// expires_at read as a valid far-future date and silently keep a token alive.
inline bool strptimeIso8601Utc(const char* s, std::tm& out)
{
#ifdef _WIN32
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, consumed = 0;
    if (std::sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ%n",
                    &y, &mo, &d, &h, &mi, &sec, &consumed) != 6)
        return false;
    if (consumed == 0 || s[consumed] != '\0') return false;   // trailing junk
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (h > 23 || mi > 59 || sec > 60) return false;          // 60 = leap second
    out.tm_year = y - 1900;
    out.tm_mon  = mo - 1;
    out.tm_mday = d;
    out.tm_hour = h;
    out.tm_min  = mi;
    out.tm_sec  = sec;
    return true;
#else
    return ::strptime(s, "%Y-%m-%dT%H:%M:%SZ", &out) != nullptr;
#endif
}

// setenv(3), overwrite-always. Windows has no setenv; _putenv_s is the CRT
// equivalent and, unlike putenv, copies the string rather than adopting the
// caller's buffer (putenv would leave a dangling pointer in the environment
// once a std::string temporary died). UCRT keeps the CRT environment and the
// Win32 environment block in sync, so a variable set here is inherited by
// CreateProcess children — which is the whole reason these call sites exist
// (LOGOS_INSTANCE_ID and LOGOSCTL_CONFIG_DIR must reach logos_host).
inline int setEnvVar(const char* name, const char* value)
{
#ifdef _WIN32
    return ::_putenv_s(name, value);
#else
    return ::setenv(name, value, 1);
#endif
}

// chmod(2) with POSIX mode bits.
//
// DELIBERATE NO-OP ON WINDOWS, and a real loss of hardening — stated here
// rather than hidden behind a call that appears to succeed:
//
//   * Windows file permissions are ACLs. The mode_t bits have no
//     representation in them, so there is nothing faithful to apply.
//   * The CRT's _chmod honours exactly one bit, _S_IWRITE (the read-only
//     attribute), and ignores owner/group/other entirely. Forwarding to it
//     would be worse than doing nothing: `chmod(path, 0400)` would mark the
//     file READ-ONLY, and the atomic-replace paths in daemon_state.cpp and
//     token_store.cpp rename over their destinations — a read-only
//     destination makes the next publish fail.
//
// So on Windows the token files, state.json and the config dir are created
// with whatever ACL they inherit from their parent directory. Under the
// default %LOCALAPPDATA% that is the user's own profile, not world-readable,
// but it is inheritance rather than enforcement: a session directory placed
// somewhere permissive via --config-dir gets that directory's ACL.
// Reproducing the 0600/0700 intent needs SetNamedSecurityInfo with an
// explicit DACL, which is not implemented.
inline int chmodPosix(const char* path, mode_t mode)
{
#ifdef _WIN32
    (void)path;
    (void)mode;
    return 0;
#else
    return ::chmod(path, mode);
#endif
}

}  // namespace logosctl

#endif  // LOGOS_PLATFORM_COMPAT_H
