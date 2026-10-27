#include <dsbox_net_http/http_module.hpp>

#include <dsbox_net/network_manager.hpp>
#include <dsbox_net_socket/connector_registry.hpp>

// PRIVATE dependency, like every other net module's use of it: only this
// .cpp knows about dsbox_config. See HttpModule's own docs for the keys.
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <charconv>
#include <chrono>
#include <stdexcept>
#include <string>

namespace dsbox_net_http
{
    namespace
    {
        template<typename T>
        T parseOr(const std::string& text, T fallback)
        {
            T value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);

            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return fallback;

            return value;
        }
    }

    HttpModule::HttpModule()
        : HttpModule(HttpOptions{})
    {
    }

    HttpModule::HttpModule(HttpOptions options)
        : m_initialOptions(std::move(options))
    {
    }

    HttpModule::~HttpModule() = default;

    void HttpModule::onInit(dsbox::Context& context)
    {
        HttpOptions options = m_initialOptions;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
        {
            options.userAgent = config->getOr("net.http", "user_agent", options.userAgent);
            options.keepAlive = config->getBool("net.http", "keep_alive", options.keepAlive);

            options.maxIdleConnectionsPerHost = parseOr<std::size_t>(
                config->getOr("net.http", "max_idle_per_host", std::to_string(options.maxIdleConnectionsPerHost)),
                options.maxIdleConnectionsPerHost);

            options.maxIdleConnections = parseOr<std::size_t>(
                config->getOr("net.http", "max_idle_total", std::to_string(options.maxIdleConnections)),
                options.maxIdleConnections);

            options.idleConnectionTimeout = std::chrono::milliseconds(parseOr<long long>(
                config->getOr("net.http", "idle_timeout_ms", std::to_string(options.idleConnectionTimeout.count())),
                options.idleConnectionTimeout.count()));

            options.connectTimeout = std::chrono::milliseconds(parseOr<long long>(
                config->getOr("net.http", "connect_timeout_ms", std::to_string(options.connectTimeout.count())),
                options.connectTimeout.count()));

            options.maxHeadBytes = parseOr<std::size_t>(
                config->getOr("net.http", "max_header_bytes", std::to_string(options.maxHeadBytes)),
                options.maxHeadBytes);

            options.bodyPipeCapacity = parseOr<std::size_t>(
                config->getOr("net.http", "body_pipe_capacity", std::to_string(options.bodyPipeCapacity)),
                options.bodyPipeCapacity);
        }

        auto net = context.get<dsbox_net::NetworkManager>();

        if (!net)
        {
            throw std::runtime_error(
                "dsbox_net_http: no dsbox_net::NetworkManager in Context -- "
                "is dsbox_net::NetModule registered as a dependency?");
        }

        auto connectors = context.get<dsbox_net_socket::ConnectorRegistry>();

        if (!connectors)
        {
            throw std::runtime_error(
                "dsbox_net_http: no dsbox_net_socket::ConnectorRegistry in Context -- "
                "is dsbox_net_socket::SocketModule registered as a dependency?");
        }

        // The resolver holds the registry itself (not a snapshot of its
        // contents), so a TLS module that starts after this one is
        // still picked up on the next request.
        auto handler = std::make_shared<HttpProtocolHandler>(
            [connectors](std::string_view scheme)
            {
                return connectors->get(scheme);
            },
            std::move(options));

        m_handler = handler;
        net->registerHandler("http", handler, this);
        net->registerHandler("https", handler, this);
    }

    void HttpModule::onStop(dsbox::Context& context)
    {
        // Stop new requests from reaching the handler first, then
        // abort what is in flight and wait for the handler's threads.
        if (auto net = context.get<dsbox_net::NetworkManager>())
            net->unregisterOwner(this);

        if (m_handler)
            m_handler->shutdown();

        m_handler.reset();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_http::HttpModule)
