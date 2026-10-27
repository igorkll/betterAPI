#include "test_framework.hpp"

#include <dsbox_net_tls_systemca/systemca_module.hpp>

#include <dsbox_net_tls/tls_module.hpp>

#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>

using namespace dsbox_net_tls_systemca_tests;
using namespace dsbox_net_tls_systemca;

namespace
{
    void testNameAndDependencies()
    {
        SystemCaModule module;
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(std::string(module.name()), "dsbox.net.tls.systemca");

        auto deps = module.dependencies();
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(deps.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(deps[0], "dsbox.net.tls");
    }

    void testOnInitThrowsWithoutTrustManagerInContextWhenEnabled()
    {
        dsbox::Context context; // no TrustManager, no config -- use_system_ca defaults to true

        SystemCaModule module;
        bool threw = false;

        try
        {
            module.onInit(context);
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_NET_TLS_SYSTEMCA_CHECK(threw);
    }

    void testDefaultsToEnabledAndRegistersAProvider()
    {
        dsbox::Context context; // no PersistentConfigStore -- use_system_ca must default to true

        dsbox_net_tls::TlsModule tlsModule;
        tlsModule.onInit(context);

        SystemCaModule module;
        module.onInit(context);

        auto trustManager = context.get<dsbox_net_tls::TrustManager>();
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(trustManager->providerCount(), static_cast<std::size_t>(1));

        module.onStop(context);
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(trustManager->providerCount(), static_cast<std::size_t>(0));
    }

    void testDisablingViaConfigRegistersNothing()
    {
        dsbox::Context context;
        context.provide<dsbox_config::PersistentConfigStore>(
            std::make_shared<dsbox_config::PersistentConfigStore>(
                dsbox_config::PersistentConfigStore::load("", "[net.tls]\nuse_system_ca=false\n")));

        dsbox_net_tls::TlsModule tlsModule;
        tlsModule.onInit(context);

        SystemCaModule module;
        module.onInit(context); // must not throw even without touching TrustManager's contents

        auto trustManager = context.get<dsbox_net_tls::TrustManager>();
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(trustManager->providerCount(), static_cast<std::size_t>(0));

        module.onStop(context); // no-op -- nothing was ever registered
    }
}

void runSystemCaModuleTests()
{
    runTest("testNameAndDependencies", testNameAndDependencies);
    runTest("testOnInitThrowsWithoutTrustManagerInContextWhenEnabled", testOnInitThrowsWithoutTrustManagerInContextWhenEnabled);
    runTest("testDefaultsToEnabledAndRegistersAProvider", testDefaultsToEnabledAndRegistersAProvider);
    runTest("testDisablingViaConfigRegistersNothing", testDisablingViaConfigRegistersNothing);
}
