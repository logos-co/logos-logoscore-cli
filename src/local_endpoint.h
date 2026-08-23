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

#include <QDir>

#include <string>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace logosctl {

// True only when a local dial for `moduleName` on `instanceId` provably cannot
// reach anyone. `pathOut`, when non-null, receives the path that was checked,
// for the error message.
//
// FAILS CLOSED. Every outcome that is merely suggestive -- a socket that
// accepts us, a connect that errors any other way, a path too long to try, a
// non-socket inode, Windows -- returns false and lets the normal dial proceed.
// A wrong `true` would refuse a reachable daemon, which is far worse than the
// twenty-second wait this exists to remove.
//
// The path is resolved the same way the dial resolves it, which is what makes
// the answer sound rather than a guess: the SDK asks for the bare server name
// `logos_<module>_<instance_id>` (LogosInstance::id), and Qt resolves a bare
// QLocalSocket/QLocalServer name against QDir::tempPath(). Both sides read
// $TMPDIR, so a daemon started under a different one is genuinely unreachable
// from here -- and saying so at once is still the right answer.
//
// Windows: always false. The local transport there is a named pipe, which
// lives in the pipe namespace rather than the temp directory and stops
// existing when its last handle closes, so this platform keeps the
// pre-existing behaviour.
inline bool localEndpointProvablyAbsent(const std::string& moduleName,
                                        const std::string& instanceId,
                                        std::string* pathOut = nullptr)
{
#ifdef _WIN32
    (void)moduleName; (void)instanceId; (void)pathOut;
    return false;
#else
    if (moduleName.empty() || instanceId.empty())
        return false;   // nothing to derive a name from

    const std::string path =
        (QDir::tempPath() + QStringLiteral("/logos_%1_%2")
             .arg(QString::fromStdString(moduleName),
                  QString::fromStdString(instanceId)))
            .toStdString();
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
