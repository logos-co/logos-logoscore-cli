#include <gtest/gtest.h>

#include "local_endpoint.h"

#include <QDir>

#include <cstring>
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
// both halves: the path derivation (against QDir::tempPath(), which is what Qt
// resolves a bare QLocalSocket/QLocalServer name against) and the liveness
// verdict for each shape the path can be in.

namespace {

std::string uniqueId(const char* suffix)
{
    return "ut" + std::to_string(::getpid()) + suffix;
}

QString endpointPath(const std::string& instanceId)
{
    return QDir::tempPath()
         + QStringLiteral("/logos_core_service_")
         + QString::fromStdString(instanceId);
}

#ifndef _WIN32
// Bind and listen at `path`. Returns the fd, or -1. Closing the fd without
// unlinking leaves exactly what a hard-killed daemon leaves: a socket inode
// with nobody behind it.
int bindListen(const QString& path)
{
    const std::string p = path.toStdString();
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
#ifdef _WIN32
    GTEST_SKIP() << "named pipes: no path is derived";
#else
    std::string path;
    logosctl::localEndpointProvablyAbsent("core_service", "abc123", &path);
    EXPECT_EQ(path, endpointPath("abc123").toStdString())
        << "the path must be the one a bare QLocalSocket name resolves to, or "
           "the check is answering a question about the wrong file";
#endif
}

TEST(LocalEndpointTest, NoSocketFileAtAll_IsProvablyAbsent)
{
    // What a clean `daemon stop` leaves: QLocalServer's destructor unlinks it.
    const std::string id = uniqueId("_gone");
    QDir().remove(endpointPath(id));

#ifdef _WIN32
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", id));
#else
    EXPECT_TRUE(logosctl::localEndpointProvablyAbsent("core_service", id));
#endif
}

#ifndef _WIN32
TEST(LocalEndpointTest, SocketFileWithNoListener_IsProvablyAbsent)
{
    // The case a stat cannot answer, and the reason this does a connect at
    // all. A hard-killed daemon leaves the inode behind, and even a clean stop
    // leaves a window between the shutdown reply and the destructor running --
    // which is exactly when someone types the next command.
    const std::string id = uniqueId("_dead");
    const QString path = endpointPath(id);

    const int fd = bindListen(path);
    ASSERT_GE(fd, 0) << "could not bind " << path.toStdString();
    ::close(fd);                       // listener gone, inode stays
    ASSERT_TRUE(QDir().exists(path)) << "the socket file should have survived";

    EXPECT_TRUE(logosctl::localEndpointProvablyAbsent("core_service", id))
        << "a socket file nobody is listening on is not a reachable daemon";

    ::unlink(path.toStdString().c_str());
}

TEST(LocalEndpointTest, LiveListener_IsNeverCalledAbsent)
{
    // The control, and the one that matters most: refusing a reachable daemon
    // is far worse than the wait this avoids.
    const std::string id = uniqueId("_live");
    const QString path = endpointPath(id);

    const int fd = bindListen(path);
    ASSERT_GE(fd, 0) << "could not bind " << path.toStdString();

    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", id));

    ::close(fd);
    ::unlink(path.toStdString().c_str());
}
#endif

TEST(LocalEndpointTest, SomeOtherFileWearingTheName_IsNotEvidence)
{
    // Only S_ISSOCK inodes get an opinion. A regular file that happens to
    // match the name says nothing about any daemon.
    const std::string id = uniqueId("_plain");
    const QString path = endpointPath(id);
    { std::ofstream ofs(path.toStdString(), std::ios::trunc); ofs << "x"; }
    ASSERT_TRUE(QDir().exists(path));

    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", id));

    QDir().remove(path);
}

TEST(LocalEndpointTest, NothingToDeriveANameFromIsNotEvidence)
{
    // A remote dial spec carries no instance_id. That says nothing about any
    // local socket, so it must not be read as "the endpoint is missing".
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("core_service", ""));
    EXPECT_FALSE(logosctl::localEndpointProvablyAbsent("", "abc123"));
}
