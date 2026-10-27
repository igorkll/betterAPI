#pragma once

#include <dsbox_net_tls_openssl/api.hpp>
#include <dsbox_net_tls_openssl/tls_connector.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_net_tls_openssl
{
    // Registers an OpenSSL-based TlsConnector for the scheme "https" in
    // the dsbox_net_socket::ConnectorRegistry, layered over the plain-TCP
    // connector that dsbox_net_socket::SocketModule registers for "http".
    // From then on dsbox_net_http fetches https:// URLs.
    //
    // Which certificates are trusted comes entirely from the
    // dsbox_net_tls::TrustManager (and so from whichever trust provider
    // modules -- dsbox_net_tls_systemca, dsbox_net_tls_localca, ... --
    // have registered themselves). With none loaded, no server is
    // trusted and every https:// fetch fails with a certificate error.
    //
    // Depends on "dsbox.net.socket" and "dsbox.net.tls".
    //
    // Configuration, all optional, from a dsbox_config::PersistentConfigStore
    // if the host provided one:
    //
    //   [net.tls.openssl]
    //   handshake_timeout_ms = 15000
    //   trust_refresh_ms     = 30000    (0 = re-read trust for every connection)
    class DSBOX_NET_TLS_OPENSSL_API OpenSslModule final : public dsbox::IModule
    {
    public:
        OpenSslModule();
        explicit OpenSslModule(TlsConnectorOptions options);
        ~OpenSslModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.tls.openssl";
        }

        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.net.socket", "dsbox.net.tls" };
        }

        void onInit(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

    private:
        TlsConnectorOptions m_initialOptions;
        std::shared_ptr<TlsConnector> m_connector;
    };
}
