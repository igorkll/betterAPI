#include "test_framework.hpp"

#include <dsbox_net_tls/tls_module.hpp>

#include <dsbox/context.hpp>

using namespace dsbox_net_tls_tests;
using namespace dsbox_net_tls;

namespace
{
    void testOnInitProvidesTrustManager()
    {
        dsbox::Context context;

        TlsModule module;
        module.onInit(context);

        auto manager = context.get<TrustManager>();
        DSBOX_NET_TLS_CHECK(manager != nullptr);
        DSBOX_NET_TLS_CHECK_EQ(manager.get(), &module.trustManager());
    }

    void testName()
    {
        TlsModule module;
        DSBOX_NET_TLS_CHECK_EQ(std::string(module.name()), "dsbox.net.tls");
    }
}

void runTlsModuleTests()
{
    runTest("testOnInitProvidesTrustManager", testOnInitProvidesTrustManager);
    runTest("testName", testName);
}
