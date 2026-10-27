#include "test_framework.hpp"

#include <dsbox_net_tls_localca/localca_module.hpp>

#include <dsbox_net_tls/tls_module.hpp>

#include <dsbox/context.hpp>

using namespace dsbox_net_tls_localca_tests;
using namespace dsbox_net_tls_localca;

namespace
{
    void testNameAndDependencies()
    {
        LocalCaModule module;
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(std::string(module.name()), "dsbox.net.tls.localca");

        auto deps = module.dependencies();
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(deps.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(deps[0], "dsbox.net.tls");
    }

    void testOnInitThrowsWithoutTrustManagerInContext()
    {
        dsbox::Context context;

        LocalCaModule module;
        bool threw = false;

        try
        {
            module.onInit(context);
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_NET_TLS_LOCALCA_CHECK(threw);
    }

    void testOnInitRegistersAProviderAndOnStopRemovesIt()
    {
        dsbox::Context context;

        dsbox_net_tls::TlsModule tlsModule;
        tlsModule.onInit(context);

        LocalCaModule module;
        module.onInit(context);

        auto trustManager = context.get<dsbox_net_tls::TrustManager>();
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(trustManager->providerCount(), static_cast<std::size_t>(1));

        module.onStop(context);
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(trustManager->providerCount(), static_cast<std::size_t>(0));
    }
}

void runLocalCaModuleTests()
{
    runTest("testNameAndDependencies", testNameAndDependencies);
    runTest("testOnInitThrowsWithoutTrustManagerInContext", testOnInitThrowsWithoutTrustManagerInContext);
    runTest("testOnInitRegistersAProviderAndOnStopRemovesIt", testOnInitRegistersAProviderAndOnStopRemovesIt);
}
