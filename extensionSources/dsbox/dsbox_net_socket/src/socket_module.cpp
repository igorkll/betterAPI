#include <dsbox_net_socket/socket_module.hpp>
#include <dsbox_net_socket/tcp_connector.hpp>

// PRIVATE dependency: only this .cpp knows about dsbox_config, exactly as
// in dsbox_net::NetModule. See SocketModule's own docs for the keys.
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <charconv>
#include <chrono>
#include <string>

namespace dsbox_net_socket
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

    SocketModule::SocketModule() = default;
    SocketModule::~SocketModule() = default;

    void SocketModule::onInit(dsbox::Context& context)
    {
        TcpConnectorOptions options;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
        {
            options.defaultTimeout = parseMillis(
                config->getOr("net.socket", "connect_timeout_ms", std::to_string(options.defaultTimeout.count())),
                options.defaultTimeout);

            options.perAddressTimeout = parseMillis(
                config->getOr("net.socket", "per_address_timeout_ms", std::to_string(options.perAddressTimeout.count())),
                options.perAddressTimeout);

            options.noDelay = config->getBool("net.socket", "tcp_no_delay", options.noDelay);
            options.keepAlive = config->getBool("net.socket", "tcp_keep_alive", options.keepAlive);
        }

        m_registry = std::make_shared<ConnectorRegistry>();
        m_registry->set("http", std::make_shared<TcpConnector>(options), this);

        context.provide<ConnectorRegistry>(m_registry);
    }

    void SocketModule::onStop(dsbox::Context& context)
    {
        (void)context;

        if (m_registry)
            m_registry->removeOwner(this);
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_socket::SocketModule)
