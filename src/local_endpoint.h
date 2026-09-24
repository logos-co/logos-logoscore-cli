#ifndef LOGOS_LOCAL_ENDPOINT_H
#define LOGOS_LOCAL_ENDPOINT_H

// Can a local-transport dial reach anyone at all?
//
// The question exists because connecting cannot answer it. A LocalSocket
// client "connects" to a socket path with no listener without complaint, QtRO
// reports nothing for an absent peer, and the RPC that follows is therefore
// neither answered nor refused -- it waits out Timeout(20000) (logos-protocol,
// cpp/logos_mode.h) and then looks exactly like a slow daemon.
//
// The pid check in Command::ensureConnected() covers the crashed-daemon case,
// where daemon/state.json is still on disk naming a dead process. It cannot
// cover the tidier one: a daemon that stopped CLEANLY removes state.json but
// leaves client/config.yaml and its token behind. From the client's side that
// session still looks dialable, and every command spent twenty seconds
// discovering otherwise.
//
// So ask the socket instead. Two facts, in order of cost:
//
//   1. Is the socket file there? A clean shutdown unlinks it (QLocalServer's
//      destructor, once the event loop returns).
//   2. If it is, does connecting get refused? The file can outlive the daemon
//      -- a hard kill leaves it, and even a clean stop leaves a window between
//      the shutdown reply and the destructor running -- so its presence is not
//      evidence of a listener. ECONNREFUSED is; that is the same signal
//      logos::isSocketDead (logos_socket_paths.h) uses to decide a socket is
//      safe for the daemon's boot reaper to unlink.
//
// Not reusing logos::isSocketDead itself is a build-graph decision, not a
// disagreement: it lives behind the logos-protocol link, and logosctl_testlib
// (tests/CMakeLists.txt) deliberately stays free of that so the command layer
// can be unit-tested without the SDK.

#include <cstdlib>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace logosctl {

// Match qt_remote_plain's resolution of a relative QLocalServer name. On
// macOS, Qt uses the per-user Darwin temp directory when TMPDIR is unset;
// std::filesystem::temp_directory_path() instead returns /tmp.
inline std::string localTransportTempDirectory()
{
    const char* configured = std::getenv("TMPDIR");
    std::string temp = configured && *configured ? configured : "";
#ifdef __APPLE__
    if (temp.empty()) {
        const std::size_t required = ::confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
        if (required > 1) {
            std::string buffer(required, '\0');
            if (::confstr(_CS_DARWIN_USER_TEMP_DIR, buffer.data(), required) > 0) {
                buffer.resize(std::strlen(buffer.c_str()));
                temp = std::move(buffer);
            }
        }
    }
#endif
    if (temp.empty()) temp = "/tmp";
    while (temp.size() > 1 && temp.back() == '/') temp.pop_back();
    return temp;
}

// True only when a local dial for `moduleName` on `instanceId` provably cannot
// reach anyone. `pathOut`, when non-null, receives the path that was checked,
// for the error message.
//
// FAILS CLOSED. Every outcome that is merely suggestive -- a socket that
// accepts us, a connect that errors any other way, a path too long to try, a
// non-socket inode, a busy pipe -- returns false and lets the normal dial proceed.
// A wrong `true` would refuse a reachable daemon, which is far worse than the
// twenty-second wait this exists to remove.
//
// The path is resolved the same way the dial resolves it, which is what makes
// the answer sound rather than a guess: the SDK asks for the bare server name
// `logos_<module>_<instance_id>` (LogosInstance::id), and Qt resolves a bare
// QLocalSocket/QLocalServer name against QDir::tempPath(). The resolver above
// also covers Qt's macOS fallback when $TMPDIR is absent. A daemon started
// under a different temp directory is genuinely unreachable from here.
//
// Windows: the local transport is a named pipe, whose name exists exactly
// while one of its server instances is open -- and the transport opens the
// next instance before closing the last, so the name never lapses while a
// server runs. A name that is not there has nobody behind it; one that is,
// busy or free, has someone.
inline bool localEndpointProvablyAbsent(const std::string& moduleName,
                                        const std::string& instanceId,
                                        std::string* pathOut = nullptr)
{
#ifdef _WIN32
    if (moduleName.empty() || instanceId.empty())
        return false;   // nothing to derive a name from

    // Named as qt_remote_plain's localSocketPath() names the bare server name.
    std::string name = "logos_" + moduleName + "_" + instanceId;
    for (char& c : name)
        if (c == '/' || c == '\\' || c == ':') c = '_';
    const std::string path = R"(\\.\pipe\)" + name;
    if (pathOut) *pathOut = path;

    // 1 ms, not 0: 0 is NMPWAIT_USE_DEFAULT_WAIT.
    if (::WaitNamedPipeA(path.c_str(), 1))
        return false;                                  // an instance is free
    return ::GetLastError() == ERROR_FILE_NOT_FOUND;   // no instance at all
#else
    if (moduleName.empty() || instanceId.empty())
        return false;   // nothing to derive a name from

    const std::string path = localTransportTempDirectory() + "/logos_" +
        moduleName + "_" + instanceId;
    if (pathOut) *pathOut = path;

    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return errno == ENOENT;   // nothing there at all: conclusive
    if (!S_ISSOCK(st.st_mode))
        return false;             // some other file wearing the name

    // sockaddr_un::sun_path is 104 bytes on macOS, 108 on Linux. A path that
    // does not fit cannot be probed -- and cannot be dialled either, but that
    // is the transport's error to report, not ours to pre-empt.
    sockaddr_un addr{};
    if (path.size() >= sizeof(addr.sun_path))
        return false;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    // Non-blocking so a listener with a full backlog cannot park a CLI command
    // here. AF_UNIX refuses instantly when nobody is listening, so the answer
    // we care about never needs a wait.
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    const int rc  = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int err = errno;
    ::close(fd);

    if (rc == 0)
        return false;                                  // someone accepted us
    return err == ECONNREFUSED || err == ENOENT;       // nobody home
#endif
}

}  // namespace logosctl

#endif  // LOGOS_LOCAL_ENDPOINT_H
