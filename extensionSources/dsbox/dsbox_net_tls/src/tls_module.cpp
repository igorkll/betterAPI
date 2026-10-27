#include <dsbox_net_tls/tls_module.hpp>

// Needed for context.provide<T>(): a template member function of
// dsbox::Context, so this .cpp (unlike tls_module.hpp, which only needs
// the forward declaration from <dsbox/module.hpp> for onInit()'s
// signature) requires the complete type.
#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

namespace dsbox_net_tls
{
    TlsModule::TlsModule() = default;
    TlsModule::~TlsModule() = default;

    void TlsModule::onInit(dsbox::Context& context)
    {
        m_trustManager = std::make_shared<TrustManager>();
        context.provide<TrustManager>(m_trustManager);
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_net_tls::TlsModule)
