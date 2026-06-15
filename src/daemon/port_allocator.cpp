#include "port_allocator.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace PortAllocator {

// Allocate on the address family matching `host`: an IPv6 literal ("::",
// "::1") binds AF_INET6, IPv4 binds AF_INET. (AF_INET-only previously returned
// 0 for IPv6 hosts, aborting startup for IPv6 TCP transports.)
uint16_t allocateEphemeralTcp(const std::string& host)
{
    const std::string h = host.empty() ? std::string("0.0.0.0") : host;

    // Try IPv6 first: if `h` parses as an IPv6 literal, bind on AF_INET6.
    in6_addr addr6{};
    if (::inet_pton(AF_INET6, h.c_str(), &addr6) == 1) {
        int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) {
            std::cerr << "PortAllocator: socket(AF_INET6) failed: "
                      << std::strerror(errno) << "\n";
            return 0;
        }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in6 sa{};
        sa.sin6_family = AF_INET6;
        sa.sin6_port   = 0;  // kernel picks
        sa.sin6_addr   = addr6;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
            std::cerr << "PortAllocator: bind() failed on [" << h << "]:0: "
                      << std::strerror(errno) << "\n";
            ::close(fd);
            return 0;
        }
        sockaddr_in6 bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
            std::cerr << "PortAllocator: getsockname() failed: "
                      << std::strerror(errno) << "\n";
            ::close(fd);
            return 0;
        }
        ::close(fd);
        return ntohs(bound.sin6_port);
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "PortAllocator: socket() failed: "
                  << std::strerror(errno) << "\n";
        return 0;
    }

    // SO_REUSEADDR so the same port can be re-bound by the child
    // immediately, even if it lingers in TIME_WAIT after our close.
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = 0;  // kernel picks
    if (::inet_pton(AF_INET, h.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "PortAllocator: inet_pton failed for '" << h
                  << "' — pass a numeric IPv4 or IPv6 address\n";
        ::close(fd);
        return 0;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "PortAllocator: bind() failed on " << h << ":0: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return 0;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
        std::cerr << "PortAllocator: getsockname() failed: "
                  << std::strerror(errno) << "\n";
        ::close(fd);
        return 0;
    }

    ::close(fd);
    return ntohs(bound.sin_port);
}

} // namespace PortAllocator
