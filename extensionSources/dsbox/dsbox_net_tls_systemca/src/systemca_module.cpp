#include <dsbox_net_tls_systemca/systemca_module.hpp>
#include <dsbox_net_tls_systemca/systemca_trust_provider.hpp>

#include <dsbox_net_tls/trust_manager.hpp>

// PRIVATE dependency, exactly like dsbox_net::NetModule's own use of
// this: only this .cpp knows about dsbox_config. See SystemCaModule's
// own docs for the recognized [net.tls] key.
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <stdexcept>

namespace dsbox_net_tls_systemca
{
    SystemCaModule::SystemCaModule() = default;
    SystemCaModule::~SystemCaModule() = default;

    void SystemCaModule::onInit(dsbox::Context& context)
    {
        bool useSystemCa = true;

        if (auto config = context.get<dsbox_config::PersistentConfigStore>())
            useSystemCa = config->getBool("net.tls", "use_system_ca", true);

        if (!useSystemCa)
            return; // this module registers nothing at all -- see its own docs

        auto trustManager = context.get<dsbox_net_tls::TrustManager>();

        if (!trustManager)
        {
            throw std::runtime_error(
                "dsbox_net_tls_systemca: no dsbox_net_tls::TrustManager in Context -- "
                "is dsbox_net_tls::TlsModule registered as a dependency?");
        }

        m_provider = std::make_shared<SystemCaTrustProvider>();
        trustManager->addProvider(m_provider, this);
    }

    void SystemCaModule::onStop(dsbox::Context& context)
    {
        if (auto trustManager = context.get<dsbox_net_tls::TrustManager>())
            trustManager->removeOwner(this);

        m_provider.reset();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_tls_systemca::SystemCaModule)
