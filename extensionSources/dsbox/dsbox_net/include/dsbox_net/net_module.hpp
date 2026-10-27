#pragma once

#include <dsbox_net/api.hpp>
#include <dsbox_net/network_manager.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string_view>

namespace dsbox_net
{
    // Publishes a NetworkManager into the module Context so any other
    // module can reach it via context.get<dsbox_net::NetworkManager>()
    // without depending on dsbox_net at link time being the same thing
    // as depending on this module at runtime -- same pattern as
    // dsbox_config's PersistentConfigStore being provide()'d by the
    // launcher.
    //
    // Configuration: NetOptions passed to the constructor are used as
    // defaults; onInit() then overrides individual fields from the
    // [net] section of a dsbox_config::PersistentConfigStore, IF one has
    // already been provide()'d into this Context by the time onInit()
    // runs (this module declares no dependency forcing that ordering --
    // config is host-provided before ModuleManager::startAll() even
    // begins, see launcher/main.cpp, so it is already there for every
    // module's onInit() regardless of dependency order). Recognized
    // keys, all optional:
    //
    //   [net]
    //   max_redirects       = 20
    //   default_timeout_ms  = 0
    //   body_pipe_capacity  = 262144
    //
    // A missing key, or no PersistentConfigStore at all, keeps whatever
    // NetOptions this module was constructed with -- so a host that
    // never sets up config.ini still gets a working NetworkManager with
    // the constructor's own defaults.
    //
    // Timeouts: if a dsbox_scheduler::Scheduler has been provide()'d into
    // the Context (by dsbox_scheduler::SchedulerModule, or by the host
    // itself) by the time onStart() runs, the NetworkManager runs its
    // request timeouts on it, sharing one timer thread with everything
    // else. This module declares no dependency on a scheduler: without
    // one, the NetworkManager uses a small private scheduler of its own
    // (see NetworkManager::useScheduler()), and everything works the same.
    // onStop() lets go of the shared scheduler again.
    class DSBOX_NET_API NetModule final : public dsbox::IModule
    {
    public:
        NetModule();
        explicit NetModule(NetOptions options);
        ~NetModule() override;

        std::string_view name() const override
        {
            return "dsbox.net";
        }

        void onInit(dsbox::Context& context) override;
        void onStart(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

        // The manager this module owns, valid from the moment onInit()
        // returns (also reachable via context.get<NetworkManager>()
        // from that point on).
        NetworkManager& manager() noexcept
        {
            return *m_manager;
        }

    private:
        NetOptions m_initialOptions;
        std::shared_ptr<NetworkManager> m_manager;
    };
}
