#pragma once

#include <dsbox_net_socket/api.hpp>
#include <dsbox_net_socket/connector_registry.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string_view>

namespace dsbox_net_socket
{
    // Publishes a ConnectorRegistry into the module Context
    // (context.get<dsbox_net_socket::ConnectorRegistry>()) and registers
    // the plain-TCP connector under the scheme "http".
    //
    // The registry is the seam between protocol modules and transports:
    // dsbox_net_http looks connectors up in it, and a TLS module (e.g.
    // dsbox_net_tls_openssl) adds its own under "https".
    //
    // Configuration, all optional, from a dsbox_config::PersistentConfigStore
    // if the host provided one:
    //
    //   [net.socket]
    //   connect_timeout_ms     = 30000
    //   per_address_timeout_ms = 10000
    //   tcp_no_delay           = true
    //   tcp_keep_alive         = true
    class DSBOX_NET_SOCKET_API SocketModule final : public dsbox::IModule
    {
    public:
        SocketModule();
        ~SocketModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.socket";
        }

        void onInit(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

    private:
        std::shared_ptr<ConnectorRegistry> m_registry;
    };
}
