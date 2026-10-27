#include <dsbox_scheduler/scheduler_module.hpp>

// PRIVATE dependency: only this .cpp knows about dsbox_config, exactly
// like dsbox_net::NetModule -- SchedulerOptions is a plain struct, so the
// public header does not change depending on whether a
// PersistentConfigStore happens to exist at runtime.
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <charconv>
#include <string>

namespace dsbox_scheduler
{
    namespace
    {
        // Parses a base-10 integer from `text`; returns `fallback` if the
        // whole string is not one. A garbled config value must never
        // crash module startup, only be ignored.
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

    SchedulerModule::SchedulerModule()
        : SchedulerModule(SchedulerOptions{})
    {
    }

    SchedulerModule::SchedulerModule(SchedulerOptions options)
        : m_initialOptions(std::move(options))
    {
    }

    SchedulerModule::~SchedulerModule() = default; // ~Scheduler() stops it

    void SchedulerModule::onInit(dsbox::Context& context)
    {
        SchedulerOptions options = m_initialOptions;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
        {
            options.threads = parseOr<std::size_t>(
                config->getOr("scheduler", "threads", std::to_string(options.threads)),
                options.threads);
        }

        m_scheduler = std::make_shared<Scheduler>(std::move(options));
        context.provide<Scheduler>(m_scheduler);
    }

    void SchedulerModule::onShutdown(dsbox::Context& context)
    {
        if (!m_scheduler)
            return;

        // Withdraw it first so nothing new picks it up while it stops --
        // unless somebody else has replaced it in the meantime.
        if (context.get<Scheduler>() == m_scheduler)
            context.provide<Scheduler>(std::shared_ptr<Scheduler>{});

        m_scheduler->stop();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_scheduler::SchedulerModule)
