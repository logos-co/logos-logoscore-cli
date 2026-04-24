#include <gtest/gtest.h>
#include <cstdlib>
#include <string>
#include "client/client_connection.h"
#include "daemon/connection_file.h"

// RAII helper: set/unset an env var for the duration of a test and put
// the previous value back (or remove it) on destruction. Keeps tests
// order-independent when several poke the same variable.
class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : m_name(name) {
        const char* prev = std::getenv(name);
        if (prev) { m_had = true; m_prev = prev; }
        if (value) setenv(name, value, 1);
        else       unsetenv(name);
    }
    ~EnvGuard() {
        if (m_had) setenv(m_name, m_prev.c_str(), 1);
        else       unsetenv(m_name);
    }
private:
    const char* m_name;
    bool        m_had = false;
    std::string m_prev;
};

TEST(ClientConnection, LocalReturnsUnchanged)
{
    // Env vars intentionally set — we still expect them to be ignored
    // for `local`, since they only make sense for tcp / tcp_ssl.
    EnvGuard h("LOGOSCORE_CLIENT_TCP_HOST", "ignored");
    EnvGuard p("LOGOSCORE_CLIENT_TCP_PORT", "9999");
    TransportInfo t{"local", "", 0, "", true, "json"};
    auto got = ClientConnection::effectiveTransport(t);
    EXPECT_EQ(got.protocol, "local");
    EXPECT_EQ(got.host, "");
    EXPECT_EQ(got.port, 0);
}

TEST(ClientConnection, TcpAppliesHostOverride)
{
    EnvGuard h("LOGOSCORE_CLIENT_TCP_HOST", "localhost");
    EnvGuard p("LOGOSCORE_CLIENT_TCP_PORT", nullptr);
    TransportInfo t{"tcp", "0.0.0.0", 6000, "", true, "json"};
    auto got = ClientConnection::effectiveTransport(t);
    EXPECT_EQ(got.host, "localhost");
    EXPECT_EQ(got.port, 6000);  // unchanged when env missing
}

TEST(ClientConnection, TcpAppliesPortOverride)
{
    EnvGuard h("LOGOSCORE_CLIENT_TCP_HOST", nullptr);
    EnvGuard p("LOGOSCORE_CLIENT_TCP_PORT", "8080");
    TransportInfo t{"tcp", "127.0.0.1", 6000, "", true, "json"};
    auto got = ClientConnection::effectiveTransport(t);
    EXPECT_EQ(got.host, "127.0.0.1");
    EXPECT_EQ(got.port, 8080);
}

TEST(ClientConnection, TcpSslAppliesNoVerifyPeer)
{
    EnvGuard np("LOGOSCORE_CLIENT_NO_VERIFY_PEER", "1");
    TransportInfo t{"tcp_ssl", "127.0.0.1", 6443, "/etc/ca.pem", true, "json"};
    auto got = ClientConnection::effectiveTransport(t);
    EXPECT_FALSE(got.verifyPeer);
}

TEST(ClientConnection, InvalidPortIgnored)
{
    EnvGuard p("LOGOSCORE_CLIENT_TCP_PORT", "not-a-number");
    TransportInfo t{"tcp", "127.0.0.1", 6000, "", true, "json"};
    auto got = ClientConnection::effectiveTransport(t);
    EXPECT_EQ(got.port, 6000);
}
