#pragma once

#include <dsbox_net_http/api.hpp>
#include <dsbox_net_http/http_protocol_handler.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_net_http
{
    // Registers an HttpProtocolHandler for "http" and "https" on the
    // dsbox_net::NetworkManager from dsbox_net::NetModule. Connections
    // come from the dsbox_net_socket::ConnectorRegistry that
    // dsbox_net_socket::SocketModule provides, looked up per request:
    //
    //   "http"  -> the plain-TCP connector SocketModule registers itself
    //   "https" -> whatever TLS module has registered one (for instance
    //              dsbox_net_tls_openssl::OpenSslModule). Without one,
    //              an https:// fetch fails with UnsupportedOperation
    //              and a message saying so; http:// keeps working.
    //
    // Depends on "dsbox.net" and "dsbox.net.socket".
    //
    // Configuration, all optional, from a dsbox_config::PersistentConfigStore
    // if the host provided one:
    //
    //   [net.http]
    //   user_agent             = DSBox/0.1
    //   keep_alive             = true
    //   max_idle_per_host      = 4
    //   max_idle_total         = 32
    //   idle_timeout_ms        = 30000
    //   connect_timeout_ms     = 0        (0 = the connector's own default)
    //   max_header_bytes       = 65536
    //   body_pipe_capacity     = 262144
    //
    // Options passed to the constructor are the defaults those keys
    // override.
    class DSBOX_NET_HTTP_API HttpModule final : public dsbox::IModule
    {
    public:
        HttpModule();
        explicit HttpModule(HttpOptions options);
        ~HttpModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.http";
        }

        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.net", "dsbox.net.socket" };
        }

        void onInit(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

    private:
        HttpOptions m_initialOptions;
        std::shared_ptr<HttpProtocolHandler> m_handler;
    };
}
