#pragma once

#include <dsbox_net_tls/api.hpp>
#include <dsbox_net_tls/trust_manager.hpp>

#include <dsbox/module.hpp>

#include <memory>
#include <string_view>

namespace dsbox_net_tls
{
    // Publishes a TrustManager into the module Context -- exactly like
    // dsbox_net::NetModule publishes a NetworkManager -- so any trust
    // source module (dsbox_net_tls_localca, dsbox_net_tls_systemca, or
    // anything else added later) can find it via
    // context.get<TrustManager>() and register itself, regardless of
    // load order (providers are independent and additive -- see
    // TrustManager's own docs).
    //
    // This module does not itself perform a TLS handshake or build an
    // OpenSSL verification context from the aggregated certificates --
    // that is a separate, later layer built on top of this one.
    class DSBOX_NET_TLS_API TlsModule final : public dsbox::IModule
    {
    public:
        TlsModule();
        ~TlsModule() override;

        std::string_view name() const override
        {
            return "dsbox.net.tls";
        }

        void onInit(dsbox::Context& context) override;

        // The manager this module owns, valid from the moment onInit()
        // returns (also reachable via context.get<TrustManager>() from
        // that point on).
        TrustManager& trustManager() noexcept
        {
            return *m_trustManager;
        }

    private:
        std::shared_ptr<TrustManager> m_trustManager;
    };
}
