#include <dsbox_net_tls_openssl/openssl_module.hpp>

#include <dsbox_net_socket/connector_registry.hpp>
#include <dsbox_net_tls/trust_manager.hpp>

// PRIVATE dependency, like every other net module's use of it. See
// OpenSslModule's own docs for the keys.
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string>

namespace dsbox_net_tls_openssl
{
    namespace
    {
        std::chrono::milliseconds parseMillis(const std::string& text, std::chrono::milliseconds fallback)
        {
            long long value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);

            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value < 0)
                return fallback;

            return std::chrono::milliseconds(value);
        }
    }

    OpenSslModule::OpenSslModule()
        : OpenSslModule(TlsConnectorOptions{})
    {
    }

    OpenSslModule::OpenSslModule(TlsConnectorOptions options)
        : m_initialOptions(std::move(options))
    {
    }

    OpenSslModule::~OpenSslModule() = default;

    void OpenSslModule::onInit(dsbox::Context& context)
    {
        TlsConnectorOptions options = m_initialOptions;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
        {
            options.handshakeTimeout = parseMillis(
                config->getOr("net.tls.openssl", "handshake_timeout_ms", std::to_string(options.handshakeTimeout.count())),
                options.handshakeTimeout);

            options.trustRefreshInterval = parseMillis(
                config->getOr("net.tls.openssl", "trust_refresh_ms", std::to_string(options.trustRefreshInterval.count())),
                options.trustRefreshInterval);
        }

        auto connectors = context.get<dsbox_net_socket::ConnectorRegistry>();

        if (!connectors)
        {
            throw std::runtime_error(
                "dsbox_net_tls_openssl: no dsbox_net_socket::ConnectorRegistry in Context -- "
                "is dsbox_net_socket::SocketModule registered as a dependency?");
        }

        auto trust = context.get<dsbox_net_tls::TrustManager>();

        if (!trust)
        {
            throw std::runtime_error(
                "dsbox_net_tls_openssl: no dsbox_net_tls::TrustManager in Context -- "
                "is dsbox_net_tls::TlsModule registered as a dependency?");
        }

        // TLS rides on the plain TCP connector registered for "http".
        auto transport = connectors->get("http");

        if (!transport)
        {
            throw std::runtime_error(
                "dsbox_net_tls_openssl: no plain-TCP connector is registered for \"http\" "
                "to layer TLS over");
        }

        m_connector = std::make_shared<TlsConnector>(std::move(transport), std::move(trust), std::move(options));
        connectors->set("https", m_connector, this);
    }

    void OpenSslModule::onStop(dsbox::Context& context)
    {
        if (auto connectors = context.get<dsbox_net_socket::ConnectorRegistry>())
            connectors->removeOwner(this);

        m_connector.reset();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_tls_openssl::OpenSslModule)
