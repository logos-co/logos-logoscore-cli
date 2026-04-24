#include "client_connection.h"

#include <cstdlib>
#include <string>

namespace ClientConnection {

TransportInfo effectiveTransport(const TransportInfo& advertised)
{
    TransportInfo t = advertised;
    if (t.protocol != "tcp" && t.protocol != "tcp_ssl")
        return t;

    if (const char* h = std::getenv("LOGOSCORE_CLIENT_TCP_HOST"))
        if (*h) t.host = h;
    if (const char* p = std::getenv("LOGOSCORE_CLIENT_TCP_PORT")) {
        if (*p) {
            // Clamp silently to avoid UB on a nonsense value; the
            // subsequent connect will surface a clearer error than a
            // cryptic range exception here.
            try {
                int v = std::stoi(p);
                if (v > 0 && v <= 65535) t.port = static_cast<uint16_t>(v);
            } catch (...) {}
        }
    }
    if (const char* np = std::getenv("LOGOSCORE_CLIENT_NO_VERIFY_PEER"))
        if (*np) t.verifyPeer = false;
    return t;
}

} // namespace ClientConnection
