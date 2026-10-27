#include "test_framework.hpp"

#include <dsbox_net_tls/trust_manager.hpp>

using namespace dsbox_net_tls_tests;
using namespace dsbox_net_tls;

namespace
{
    class FakeModule final : public dsbox::IModule
    {
    public:
        explicit FakeModule(std::string moduleName) : m_name(std::move(moduleName)) {}
        std::string_view name() const override { return m_name; }

    private:
        std::string m_name;
    };

    class FixedProvider final : public ITrustProvider
    {
    public:
        explicit FixedProvider(std::size_t count) : m_count(count) {}

        std::vector<Certificate> trustedCertificates() const override
        {
            std::vector<Certificate> result(m_count);
            return result;
        }

    private:
        std::size_t m_count;
    };

    void testCollectAllConcatenatesEveryProvider()
    {
        TrustManager manager;
        manager.addProvider(std::make_shared<FixedProvider>(2));
        manager.addProvider(std::make_shared<FixedProvider>(3));

        DSBOX_NET_TLS_CHECK_EQ(manager.providerCount(), static_cast<std::size_t>(2));
        DSBOX_NET_TLS_CHECK_EQ(manager.collectAll().size(), static_cast<std::size_t>(5));
    }

    void testRemoveProviderById()
    {
        TrustManager manager;
        const auto id = manager.addProvider(std::make_shared<FixedProvider>(2));
        manager.addProvider(std::make_shared<FixedProvider>(3));

        DSBOX_NET_TLS_CHECK(manager.removeProvider(id));
        DSBOX_NET_TLS_CHECK_EQ(manager.collectAll().size(), static_cast<std::size_t>(3));

        DSBOX_NET_TLS_CHECK(!manager.removeProvider(id)); // already gone
    }

    void testRemoveOwnerRemovesOnlyItsProviders()
    {
        TrustManager manager;
        FakeModule owner("test.owner");

        manager.addProvider(std::make_shared<FixedProvider>(1), &owner);
        manager.addProvider(std::make_shared<FixedProvider>(1), &owner);
        manager.addProvider(std::make_shared<FixedProvider>(1), dsbox::Anonymous);

        const auto removed = manager.removeOwner(&owner);

        DSBOX_NET_TLS_CHECK_EQ(removed, static_cast<std::size_t>(2));
        DSBOX_NET_TLS_CHECK_EQ(manager.providerCount(), static_cast<std::size_t>(1));
    }

    void testNoRegisteredProvidersYieldsEmptyResult()
    {
        TrustManager manager;
        DSBOX_NET_TLS_CHECK(manager.collectAll().empty());
    }

    void testProviderCanRegisterAnotherProviderFromWithinItself()
    {
        // Guards against collectAll() holding its lock while calling
        // into a provider -- see TrustManager::collectAll()'s own docs.
        TrustManager manager;

        class ReentrantProvider final : public ITrustProvider
        {
        public:
            explicit ReentrantProvider(TrustManager& manager) : m_manager(manager) {}

            std::vector<Certificate> trustedCertificates() const override
            {
                m_manager.addProvider(std::make_shared<FixedProvider>(1));
                return {};
            }

        private:
            TrustManager& m_manager;
        };

        manager.addProvider(std::make_shared<ReentrantProvider>(manager));

        auto result = manager.collectAll(); // must not deadlock
        DSBOX_NET_TLS_CHECK(result.empty()); // the newly-added provider wasn't part of THIS snapshot
        DSBOX_NET_TLS_CHECK_EQ(manager.providerCount(), static_cast<std::size_t>(2));
    }
}

void runTrustManagerTests()
{
    runTest("testCollectAllConcatenatesEveryProvider", testCollectAllConcatenatesEveryProvider);
    runTest("testRemoveProviderById", testRemoveProviderById);
    runTest("testRemoveOwnerRemovesOnlyItsProviders", testRemoveOwnerRemovesOnlyItsProviders);
    runTest("testNoRegisteredProvidersYieldsEmptyResult", testNoRegisteredProvidersYieldsEmptyResult);
    runTest("testProviderCanRegisterAnotherProviderFromWithinItself", testProviderCanRegisterAnotherProviderFromWithinItself);
}
