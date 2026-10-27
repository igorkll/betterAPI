#pragma once

#include <dsbox_net_tls_systemca/api.hpp>

#include <dsbox_net_tls/trust_provider.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string_view>

namespace dsbox_net_tls_systemca
{
    // Registers a SystemCaTrustProvider with the dsbox_net_tls::TrustManager
    // provided by dsbox_net_tls::TlsModule -- depends on "dsbox.net.tls"
    // (see dependencies()) for that.
    //
    // This is the ONE place the UseSystemCA toggle lives: onInit() reads
    // `use_system_ca` from the [net.tls] section of a
    // dsbox_config::PersistentConfigStore, IF one is available in the
    // Context by then, defaulting to true if there's no store or the key
    // is absent. When it resolves to false, this module simply does NOT
    // register a provider at all -- it doesn't enumerate the OS store,
    // doesn't create a SystemCaTrustProvider, doesn't touch TrustManager.
    // dsbox_net_tls::TrustManager itself has no idea this toggle exists
    // (see its own docs on providers being independent) -- as far as it
    // is concerned, this module either registers a provider or it
    // doesn't.
    //
    //   [net.tls]
    //   use_system_ca = true | false      (default: true)
    class DSBOX_NET_TLS_SYSTEMCA_API SystemCaModule final : public dsbox::IModule
    {
    public:
        SystemCaModule();
        ~SystemCaModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.tls.systemca";
        }

        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.net.tls" };
        }

        void onInit(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

    private:
        std::shared_ptr<dsbox_net_tls::ITrustProvider> m_provider; // null if use_system_ca resolved to false
    };
}
