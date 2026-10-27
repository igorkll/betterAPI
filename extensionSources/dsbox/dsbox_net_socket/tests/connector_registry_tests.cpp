#include "test_framework.hpp"

#include <dsbox_net_socket/connector_registry.hpp>

#include <memory>

using namespace dsbox_net_socket_tests;
using namespace dsbox_net_socket;

namespace
{
    class FakeConnector final : public IConnector
    {
    public:
        std::shared_ptr<IConnection> connect(const std::string&, std::uint16_t, const ConnectOptions&) override
        {
            return nullptr;
        }
    };

    // Two distinct addresses to use as owner identities; the registry
    // only ever compares them, never dereferences them.
    class DummyModule final : public dsbox::IModule
    {
    public:
        std::string_view name() const override { return "dummy"; }
    };

    void testSetAndGet()
    {
        ConnectorRegistry registry;
        auto connector = std::make_shared<FakeConnector>();

        DSBOX_NET_SOCKET_CHECK(registry.get("http") == nullptr);

        registry.set("http", connector);

        DSBOX_NET_SOCKET_CHECK(registry.get("http") == connector);
    }

    void testSchemeIsCaseInsensitive()
    {
        ConnectorRegistry registry;
        auto connector = std::make_shared<FakeConnector>();

        registry.set("HTTPS", connector);

        DSBOX_NET_SOCKET_CHECK(registry.get("https") == connector);
        DSBOX_NET_SOCKET_CHECK(registry.get("HtTpS") == connector);
    }

    void testSetReplacesExisting()
    {
        ConnectorRegistry registry;
        auto first = std::make_shared<FakeConnector>();
        auto second = std::make_shared<FakeConnector>();

        registry.set("http", first);
        registry.set("http", second);

        DSBOX_NET_SOCKET_CHECK(registry.get("http") == second);
    }

    void testRemove()
    {
        ConnectorRegistry registry;
        registry.set("http", std::make_shared<FakeConnector>());

        DSBOX_NET_SOCKET_CHECK(registry.remove("http"));
        DSBOX_NET_SOCKET_CHECK(!registry.remove("http"));
        DSBOX_NET_SOCKET_CHECK(registry.get("http") == nullptr);
    }

    void testRemoveOwnerOnlyRemovesThatOwnersEntries()
    {
        ConnectorRegistry registry;
        DummyModule ownerA;
        DummyModule ownerB;

        registry.set("http", std::make_shared<FakeConnector>(), &ownerA);
        registry.set("https", std::make_shared<FakeConnector>(), &ownerA);
        registry.set("ftp", std::make_shared<FakeConnector>(), &ownerB);

        DSBOX_NET_SOCKET_CHECK_EQ(registry.removeOwner(&ownerA), static_cast<std::size_t>(2));
        DSBOX_NET_SOCKET_CHECK(registry.get("http") == nullptr);
        DSBOX_NET_SOCKET_CHECK(registry.get("https") == nullptr);
        DSBOX_NET_SOCKET_CHECK(registry.get("ftp") != nullptr);
    }
}

void runConnectorRegistryTests()
{
    runTest("testSetAndGet", testSetAndGet);
    runTest("testSchemeIsCaseInsensitive", testSchemeIsCaseInsensitive);
    runTest("testSetReplacesExisting", testSetReplacesExisting);
    runTest("testRemove", testRemove);
    runTest("testRemoveOwnerOnlyRemovesThatOwnersEntries", testRemoveOwnerOnlyRemovesThatOwnersEntries);
}
