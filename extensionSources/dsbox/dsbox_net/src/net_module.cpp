#include <dsbox_net/net_module.hpp>

// PRIVATE dependency: only this .cpp knows about dsbox_config. The
// public net_module.hpp contract (NetOptions is a plain struct) does
// not change depending on whether a PersistentConfigStore happens to be
// available at runtime -- see net_module.hpp's own docs.
#include <dsbox_config/persistent_config_store.hpp>

// Needed for context.get<T>()/context.provide<T>(): those are template
// member functions of dsbox::Context, so this .cpp (unlike net_module.hpp,
// which only needs the forward declaration from <dsbox/module.hpp> for
// onInit()'s signature) requires the complete type.
#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <dsbox_scheduler/scheduler.hpp>

#include <charconv>
#include <chrono>
#include <string>

namespace dsbox_net
{
    namespace
    {
        // Parses a base-10 integer from `text`; returns `fallback`
        // unchanged if `text` isn't one (whole string must match --
        // trailing garbage also falls back). A blank/garbled config
        // value should never crash module startup, just be ignored.
        template<typename T>
        T parseOr(std::string_view text, T fallback)
        {
            T value{};

            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);

            if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
                return fallback;

            return value;
        }
    }

    NetModule::NetModule()
        : NetModule(NetOptions{})
    {
    }

    NetModule::NetModule(NetOptions options)
        : m_initialOptions(options)
    {
    }

    NetModule::~NetModule() = default;

    void NetModule::onInit(dsbox::Context& context)
    {
        NetOptions options = m_initialOptions;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
        {
            options.maxRedirects = parseOr<int>(
                config->getOr("net", "max_redirects", std::to_string(options.maxRedirects)),
                options.maxRedirects);

            const auto timeoutMs = parseOr<std::chrono::milliseconds::rep>(
                config->getOr(
                    "net",
                    "default_timeout_ms",
                    std::to_string(options.defaultTimeout.count())),
                options.defaultTimeout.count());
            options.defaultTimeout = std::chrono::milliseconds(timeoutMs);

            options.bodyPipeCapacity = parseOr<std::size_t>(
                config->getOr("net", "body_pipe_capacity", std::to_string(options.bodyPipeCapacity)),
                options.bodyPipeCapacity);

            options.stripCredentialsOnCrossOriginRedirect = config->getBool(
                "net",
                "strip_credentials_on_cross_origin_redirect",
                options.stripCredentialsOnCrossOriginRedirect);
        }

        m_manager = std::make_shared<NetworkManager>(options);
        context.provide<NetworkManager>(m_manager);
    }

    void NetModule::onStart(dsbox::Context& context)
    {
        // Every module's onInit() has finished by now (ModuleManager runs
        // the two phases back to back across ALL modules), so a scheduler
        // module's provide() is visible regardless of dependency order --
        // which is why no dependency on it needs declaring. Absent is fine.
        if (m_manager)
            m_manager->useScheduler(context.get<dsbox_scheduler::Scheduler>());
    }

    void NetModule::onStop(dsbox::Context&)
    {
        // Let go before a scheduler module's onShutdown() stops it.
        if (m_manager)
            m_manager->useScheduler(nullptr);
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net::NetModule)
