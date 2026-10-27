#pragma once

#include <dsbox_net_socket/api.hpp>
#include <dsbox_net_socket/connection.hpp>

#include <chrono>

namespace dsbox_net_socket
{
    struct TcpConnectorOptions
    {
        // Used when ConnectOptions::timeout is zero.
        std::chrono::milliseconds defaultTimeout{ 30000 };

        // Cap on a single address attempt, so one dead address (a
        // black-holed IPv6 route, say) does not eat the whole budget
        // before the next address is even tried.
        std::chrono::milliseconds perAddressTimeout{ 10000 };

        // TCP_NODELAY on new connections. Request/response protocols
        // want it; bulk-transfer ones may not.
        bool noDelay = true;

        // SO_KEEPALIVE on new connections.
        bool keepAlive = true;
    };

    // Plain TCP over the operating system's sockets (Winsock on
    // Windows, BSD sockets elsewhere). IPv4 and IPv6; tries each address
    // the resolver returns, in order.
    //
    // Name resolution uses the system's blocking getaddrinfo(), which
    // cannot be interrupted: a cancel during a slow lookup is noticed
    // when the lookup returns.
    class DSBOX_NET_SOCKET_API TcpConnector final : public IConnector
    {
    public:
        TcpConnector();
        explicit TcpConnector(TcpConnectorOptions options);
        ~TcpConnector() override;

        std::shared_ptr<IConnection> connect(
            const std::string& host,
            std::uint16_t port,
            const ConnectOptions& options) override;

    private:
        TcpConnectorOptions m_options;
    };
}
