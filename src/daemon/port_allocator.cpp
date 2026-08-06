#include "port_allocator.h"

#ifdef _WIN32
// winsock2.h must precede windows.h; including it first here guarantees that
// regardless of what else pulls windows.h in later in the TU.
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// One socket abstraction rather than an #ifdef at each of the twelve call
// sites. Windows sockets are not file descriptors: the handle type is
// unsigned, the failure sentinel is INVALID_SOCKET rather than -1 (so `< 0`
// is never true and a failed socket() would sail straight into bind()),
// close() does not accept one, and errors land in WSAGetLastError() rather
// than errno.
#ifdef _WIN32
using socket_t = SOCKET;
using socklen_arg_t = int;
inline bool socketValid(socket_t s) { return s != INVALID_SOCKET; }
inline void closeSocket(socket_t s) { ::closesocket(s); }
inline std::string socketError() { return "WSA error " + std::to_string(::WSAGetLastError()); }

// Winsock refuses every call until WSAStartup has run in this process. Qt does
// call it, but PortAllocator is also exercised by a unit test with no
// QCoreApplication, so do not depend on that. Refcounted, so an extra pair is
// harmless; deliberately never cleaned up, since the process needs sockets for
// its whole life.
void ensureWinsock()
{
    static const bool once = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void)once;
}
#else
using socket_t = int;
using socklen_arg_t = socklen_t;
inline bool socketValid(socket_t s) { return s >= 0; }
inline void closeSocket(socket_t s) { ::close(s); }
inline std::string socketError() { return std::strerror(errno); }
inline void ensureWinsock() {}
#endif

// Ask for the port only if nobody else holds it.
//
// SO_REUSEADDR does NOT mean the same thing on the two platforms. On POSIX it
// lets a fresh bind step past a TIME_WAIT remnant. On Windows it lets a second
// socket bind a port that is ALREADY BOUND AND LISTENING -- the live listener
// keeps running and which socket receives a given connection is undefined.
// Using it here would let this probe hand out a port another process is
// serving on, and the daemon would advertise that port in state.json.
// SO_EXCLUSIVEADDRUSE is the option that carries the POSIX intent on Windows.
// (TIME_WAIT is not a concern for this probe either way: the socket is never
// listened on and never accepts, so it leaves no TIME_WAIT entry behind.)
void setReuseOption(socket_t fd)
{
    const int one = 1;
#ifdef _WIN32
    ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&one), sizeof(one));
#else
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
}

}  // namespace

namespace PortAllocator {

// Allocate on the address family matching `host`: an IPv6 literal ("::",
// "::1") binds AF_INET6, IPv4 binds AF_INET. (AF_INET-only previously returned
// 0 for IPv6 hosts, aborting startup for IPv6 TCP transports.)
uint16_t allocateEphemeralTcp(const std::string& host)
{
    ensureWinsock();

    const std::string h = host.empty() ? std::string("0.0.0.0") : host;

    // Try IPv6 first: if `h` parses as an IPv6 literal, bind on AF_INET6.
    in6_addr addr6{};
    if (::inet_pton(AF_INET6, h.c_str(), &addr6) == 1) {
        socket_t fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (!socketValid(fd)) {
            std::cerr << "PortAllocator: socket(AF_INET6) failed: "
                      << socketError() << "\n";
            return 0;
        }
        setReuseOption(fd);
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_port   = 0;  // kernel picks
        sa.sin6_addr   = addr6;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            std::cerr << "PortAllocator: bind() failed on [" << h << "]:0: "
                      << socketError() << "\n";
            closeSocket(fd);
            return 0;
        }
        sockaddr_in6 bound{};
        socklen_arg_t len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            std::cerr << "PortAllocator: getsockname() failed: "
                      << socketError() << "\n";
            closeSocket(fd);
            return 0;
        }
        closeSocket(fd);
        return ntohs(bound.sin6_port);
    }

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!socketValid(fd)) {
        std::cerr << "PortAllocator: socket() failed: "
                  << socketError() << "\n";
        return 0;
    }

    setReuseOption(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;  // kernel picks
    if (::inet_pton(AF_INET, h.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "PortAllocator: inet_pton failed for '" << h
                  << "' — pass a numeric IPv4 or IPv6 address\n";
        closeSocket(fd);
        return 0;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "PortAllocator: bind() failed on " << h << ":0: "
                  << socketError() << "\n";
        closeSocket(fd);
        return 0;
    }

    sockaddr_in bound{};
    socklen_arg_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        std::cerr << "PortAllocator: getsockname() failed: "
                  << socketError() << "\n";
        closeSocket(fd);
        return 0;
    }

    closeSocket(fd);
    return ntohs(bound.sin_port);
}

} // namespace PortAllocator
