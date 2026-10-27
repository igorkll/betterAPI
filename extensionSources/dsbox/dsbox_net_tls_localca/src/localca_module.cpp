#include <dsbox_net_tls_localca/localca_module.hpp>
#include <dsbox_net_tls_localca/localca_trust_provider.hpp>

#include <dsbox_net_tls/trust_manager.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <stdexcept>

namespace dsbox_net_tls_localca
{
    LocalCaModule::LocalCaModule()
        : LocalCaModule(LocalCaModuleOptions{})
    {
    }

    LocalCaModule::LocalCaModule(LocalCaModuleOptions options)
        : m_options(std::move(options))
    {
    }

    LocalCaModule::~LocalCaModule() = default;

    void LocalCaModule::onInit(dsbox::Context& context)
    {
        auto trustManager = context.get<dsbox_net_tls::TrustManager>();

        if (!trustManager)
        {
            throw std::runtime_error(
                "dsbox_net_tls_localca: no dsbox_net_tls::TrustManager in Context -- "
                "is dsbox_net_tls::TlsModule registered as a dependency?");
        }

        m_provider = std::make_shared<LocalCaTrustProvider>(
            context.comm(),
            m_options.resolveDirOverrideName,
            m_options.readDirOverrideName);

        trustManager->addProvider(m_provider, this);
    }

    void LocalCaModule::onStop(dsbox::Context& context)
    {
        if (auto trustManager = context.get<dsbox_net_tls::TrustManager>())
            trustManager->removeOwner(this);

        m_provider.reset();
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_tls_localca::LocalCaModule)
