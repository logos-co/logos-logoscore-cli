#include <gtest/gtest.h>
#include "test_platform.h"

#include "local_endpoint.h"

#include <qtro_transport.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// localEndpointProvablyAbsent() is what turns "a daemon that stopped" from a
// twenty-second wait into an immediate answer, and it is the one piece of this
// that could refuse a LIVE daemon if it got either half wrong. So these pin
// both halves: the path derivation (against where the daemon's transport
// binds) and the liveness verdict for each shape the path can be in.

namespace {

std::string uniqueId(const char* suffix)
{
    return "ut" + std::to_string(logosctl_test::currentPid()) + suffix;
}

// Where the daemon's own transport binds the endpoint: the oracle, never the
// resolver under test.
std::filesystem::path endpointPath(const std::string& instanceId)
{
    return logos::qt_remote_plain::localSocketPath("logos_core_service_" + instanceId);
}

#ifndef _WIN32
// Bind and listen at `path`. Returns the fd, or -1. Closing the fd without
// unlinking leaves exactly what a hard-killed daemon leaves: a socket inode
// with nobody behind it.
int bindListen(const std::filesystem::path& path)
{
    const std::string p = path.string();
    sockaddr_un addr{};
    if (p.size() >= sizeof(addr.sun_path)) return -1;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, p.c_str(), p.size());

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    ::unlink(p.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
#endif

} // namespace

TEST(LocalEndpointTest, ReportsTheDerivedPathItChecked)
{
    std::string path;
    logosctl::localEndpointProvablyAbsent("core_service", "abc123", &path);
    EXPECT_EQ(path, endpointPath("abc123").string())
        << "the path must be the one a bare QLocalSocket name resolves to, or "
           "the check is answering a question about the wrong file";
}

#ifdef __APPLE__
TEST(LocalEndpointTest, UsesDarwinUserTempWhenTmpdirIsUnset)
{
    struct TmpdirGuard {
        const char* previous = std::getenv("TMPDIR");
        std::string saved = previous ? previous : "";
        bool hadPrevious = previous != nullptr;
        TmpdirGuard() { logosctl_test::unsetEnv("TMPDIR"); }
        ~TmpdirGuard() {
            if (hadPrevious) logosctl_test::setEnv("TMPDIR", saved);
            else logosctl_test::unsetEnv("TMPDIR");
        }
    } guard;

    const std::size_t required = ::confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
    ASSERT_GT(required, 1u);
    std::string expected(required, '\0');
    ASSERT_GT(::confstr(_CS_DARWIN_USER_TEMP_DIR, expected.data(), required), 0u);
    expected.resize(std::strlen(expected.c_str()));
    while (expected.size() > 1 && expected.back() == '/') expected.pop_back();

    const std::string id = uniqueId("_darwin");
    logos::qt_remote_plain::Server daemon;
    std::string error;
    ASSERT_TRUE(daemon.start("local:logos_core_service_" + id, &error)) << error;
    ASSERT_EQ(std::filesystem::path(daemon.socketPath()).parent_path(),
              std::filesystem::path(expected));

    std::string checked;
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", id, &checked));
    EXPECT_EQ(checked, daemon.socketPath());
}
#endif

TEST(LocalEndpointTest, NoSocketFileAtAll_IsProvablyAbsent)
{
    // What a clean `daemon stop` leaves: QLocalServer's destructor unlinks it.
    // On Windows the pipe name goes with the server's last instance.
    const std::string id = uniqueId("_gone");
    std::error_code ec;
    std::filesystem::remove(endpointPath(id), ec);

    EXPECT_TRUE(logosctl::localEndpointProvablyAbsent("core_service", id));
}

#ifndef _WIN32
TEST(LocalEndpointTest, SocketFileWithNoListener_IsProvablyAbsent)
{
    // The case a stat cannot answer, and the reason this does a connect at
    // all. A hard-killed daemon leaves the inode behind, and even a clean stop
    // leaves a window between the shutdown reply and the destructor running --
    // which is exactly when someone types the next command.
    const std::string id = uniqueId("_dead");
    const std::filesystem::path path = endpointPath(id);

    const int fd = bindListen(path);
    ASSERT_GE(fd, 0) << "could not bind " << path.string();
    ::close(fd);                       // listener gone, inode stays
    ASSERT_TRUE(std::filesystem::exists(path)) << "the socket file should have survived";

    EXPECT_TRUE(logosctl::localEndpointProvablyAbsent("core_service", id))
        << "a socket file nobody is listening on is not a reachable daemon";

    ::unlink(path.string().c_str());
}
#endif

TEST(LocalEndpointTest, LiveListener_IsNeverCalledAbsent)
{
    // The control, and the one that matters most: refusing a reachable daemon
    // is far worse than the wait this avoids. The listener is the transport's
    // own, so it sits where a daemon's would.
    const std::string id = uniqueId("_live");
    logos::qt_remote_plain::Server daemon;
    std::string error;
    ASSERT_TRUE(daemon.start("local:logos_core_service_" + id, &error)) << error;

    std::string checked;
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", id, &checked));
    EXPECT_EQ(checked, daemon.socketPath());
}

#ifndef _WIN32
// A named pipe has no file to wear its name on Windows.
TEST(LocalEndpointTest, SomeOtherFileWearingTheName_IsNotEvidence)
{
    // Only S_ISSOCK inodes get an opinion. A regular file that happens to
    // match the name says nothing about any daemon.
    const std::string id = uniqueId("_plain");
    const std::filesystem::path path = endpointPath(id);
    { std::ofstream ofs(path, std::ios::trunc); ofs << "x"; }
    ASSERT_TRUE(std::filesystem::exists(path));

    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", id));

    std::filesystem::remove(path);
}
#endif

TEST(LocalEndpointTest, NothingToDeriveANameFromIsNotEvidence)
{
    // A remote dial spec carries no instance_id. That says nothing about any
    // local socket, so it must not be read as "the endpoint is missing".
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", ""));
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("", "abc123"));
}
