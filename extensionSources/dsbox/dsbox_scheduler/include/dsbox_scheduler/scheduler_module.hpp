#pragma once

#include <dsbox_scheduler/api.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string_view>

namespace dsbox_scheduler
{
    // Publishes ONE shared Scheduler into the module Context, for modules
    // that want a common timer service:
    //
    //   if (auto scheduler = context.get<dsbox_scheduler::Scheduler>())
    //       scheduler->after(1s, [] { ... });
    //
    // This module is entirely optional. Scheduler itself has no
    // dependency on the module system -- it lives in its own library
    // (DSBox::Scheduler, which does not even link dsbox_core), while this
    // wrapper is in DSBox::SchedulerModule. A module (or anything else)
    // that needs timers can just own its own Scheduler, and a module that
    // runs its own thread can do without one. So:
    //
    //  - do not add "dsbox.scheduler" to dependencies() unless you cannot
    //    work without it, and
    //  - always handle context.get<Scheduler>() returning null.
    //
    // Nothing is started at init: the Scheduler creates its timer thread
    // and worker pool lazily, on first use.
    //
    // Configuration: SchedulerOptions passed to the constructor are the
    // defaults; onInit() then overrides fields from the [scheduler]
    // section of a dsbox_config::PersistentConfigStore, if one has been
    // provide()'d (same rules as dsbox_net::NetModule). Recognized keys,
    // all optional:
    //
    //   [scheduler]
    //   threads = 0        ; size of the default worker pool, 0 = one per core
    //
    // Shutdown: onShutdown() stops the Scheduler (see Scheduler::stop())
    // and withdraws it from the Context.
    class DSBOX_SCHEDULER_MODULE_API SchedulerModule final : public dsbox::IModule
    {
    public:
        SchedulerModule();
        explicit SchedulerModule(SchedulerOptions options);
        ~SchedulerModule() override;

        std::string_view name() const override
        {
            return "dsbox.scheduler";
        }

        void onInit(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Valid from the moment onInit() returns.
        Scheduler& scheduler() noexcept
        {
            return *m_scheduler;
        }

    private:
        SchedulerOptions m_initialOptions;
        std::shared_ptr<Scheduler> m_scheduler;
    };
}
