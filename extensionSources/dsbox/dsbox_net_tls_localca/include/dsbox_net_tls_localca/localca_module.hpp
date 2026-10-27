#pragma once

#include <dsbox_net_tls_localca/api.hpp>

#include <dsbox_net_tls/trust_provider.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_net_tls_localca
{
    struct LocalCaModuleOptions
    {
        // Names of the two dsbox::Overridable slots this module's
        // provider uses -- see LocalCaTrustProvider's own docs for their
        // signatures and default (real-filesystem) behavior. Override
        // either on the shared bus (context.comm()) to sandbox/fake
        // local CA loading, most usefully in tests.
        std::string resolveDirOverrideName = "net.tls.localca.resolve_dir";
        std::string readDirOverrideName = "net.tls.localca.read_dir";
    };

    // Registers a LocalCaTrustProvider with the dsbox_net_tls::TrustManager
    // provided by dsbox_net_tls::TlsModule -- depends on "dsbox.net.tls"
    // (see dependencies()) for that. No configuration is read from
    // dsbox_config: unlike dsbox_net_tls_systemca's UseSystemCA, there is
    // no toggle here -- the two conventional directories are always both
    // read (an empty/missing directory simply contributes nothing; see
    // LocalCaTrustProvider's own docs).
    class DSBOX_NET_TLS_LOCALCA_API LocalCaModule final : public dsbox::IModule
    {
    public:
        LocalCaModule();
        explicit LocalCaModule(LocalCaModuleOptions options);
        ~LocalCaModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.tls.localca";
        }

        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.net.tls" };
        }

        void onInit(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;

    private:
        LocalCaModuleOptions m_options;
        std::shared_ptr<dsbox_net_tls::ITrustProvider> m_provider;
    };
}
