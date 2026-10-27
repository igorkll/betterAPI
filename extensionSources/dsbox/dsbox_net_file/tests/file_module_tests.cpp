#include "test_framework.hpp"

#include <dsbox_net_file/file_module.hpp>
#include <dsbox_net_file/file_protocol_handler.hpp>

#include <dsbox_net/network_manager.hpp>

#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox/context.hpp>

using namespace dsbox_net_file_tests;
using namespace dsbox_net_file;
using namespace dsbox_net;

namespace
{
    void testNameAndDependencies()
    {
        FileModule module;
        DSBOX_NET_FILE_CHECK_EQ(std::string(module.name()), "dsbox.net.file");

        auto deps = module.dependencies();
        DSBOX_NET_FILE_CHECK_EQ(deps.size(), static_cast<std::size_t>(1));
        DSBOX_NET_FILE_CHECK_EQ(deps[0], "dsbox.net");
    }

    void testOnInitThrowsWithoutNetworkManagerInContext()
    {
        dsbox::Context context; // NetworkManager never provide()'d

        FileModule module;
        bool threw = false;

        try
        {
            module.onInit(context);
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_NET_FILE_CHECK(threw);
    }

    void testOnInitRegistersFileSchemeOnNetworkManager()
    {
        dsbox::Context context;
        context.provide<NetworkManager>(std::make_shared<NetworkManager>());

        FileModule module;
        module.onInit(context);

        auto net = context.get<NetworkManager>();
        DSBOX_NET_FILE_CHECK(net != nullptr);
        DSBOX_NET_FILE_CHECK(net->supports("file"));
    }

    void testOnStopUnregistersFileScheme()
    {
        dsbox::Context context;
        context.provide<NetworkManager>(std::make_shared<NetworkManager>());

        FileModule module;
        module.onInit(context);
        module.onStop(context);

        auto net = context.get<NetworkManager>();
        DSBOX_NET_FILE_CHECK(!net->supports("file"));
    }

    void testExplicitPathStyleBypassesConfig()
    {
        dsbox::Context context; // no PersistentConfigStore -- options must survive untouched
        context.provide<NetworkManager>(std::make_shared<NetworkManager>());

        FileModuleOptions options;
        options.pathStyle = FilePathStyle::WindowsOnly; // explicit, not Default
        options.overrideName = "custom.file.open";

        FileModule module(options);
        module.onInit(context);

        auto net = context.get<NetworkManager>();
        DSBOX_NET_FILE_CHECK(net->supports("file"));

        // Indirect check: a POSIX-shaped path should now be rejected
        // (StatusClass::NotFound) since WindowsOnly was requested
        // explicitly -- an explicit, non-Default style never looks at
        // configuration at all (see FileModuleOptions::pathStyle's own
        // docs), so this doesn't depend on there being no config store.
        auto result = net->fetch(Request{ .url = *Url::parse("file:///home/user/file.txt") }).get();
        DSBOX_NET_FILE_CHECK(result.ok());
        DSBOX_NET_FILE_CHECK(result.response.statusClass == StatusClass::NotFound);
    }

    void testDefaultPathStyleFallsBackToPosixWithoutConfig()
    {
        dsbox::Context context; // no PersistentConfigStore at all
        context.provide<NetworkManager>(std::make_shared<NetworkManager>());

        std::string observedPath;
        context.comm().registerHandler<FileOpenSignature>(
            "net.file.open",
            [&](std::string path) -> FileOpenResult
            {
                observedPath = path;
                FileOpenResult result;
                result.failure = FileOpenResult::Failure::NotFound;
                return result;
            });

        FileModule module; // FileModuleOptions{} -- pathStyle == Default
        module.onInit(context);

        auto net = context.get<NetworkManager>();
        net->fetch(Request{ .url = *Url::parse("file:///C:/test") }).get();

        // Resolved to PosixOnly (no PersistentConfigStore in the
        // Context) -- "/C:/test" must reach the override literally, NOT
        // converted to "C:\test".
        DSBOX_NET_FILE_CHECK_EQ(observedPath, "/C:/test");

        module.onStop(context);
    }

    void testDefaultPathStyleIsReadFromConfig()
    {
        dsbox::Context context;
        context.provide<NetworkManager>(std::make_shared<NetworkManager>());
        context.provide<dsbox_config::PersistentConfigStore>(
            std::make_shared<dsbox_config::PersistentConfigStore>(
                dsbox_config::PersistentConfigStore::load("", "[net.file]\npath_style=windows\n")));

        std::string observedPath;
        context.comm().registerHandler<FileOpenSignature>(
            "net.file.open",
            [&](std::string path) -> FileOpenResult
            {
                observedPath = path;
                FileOpenResult result;
                result.failure = FileOpenResult::Failure::NotFound;
                return result;
            });

        FileModule module; // FileModuleOptions{} -- pathStyle == Default
        module.onInit(context);

        auto net = context.get<NetworkManager>();
        net->fetch(Request{ .url = *Url::parse("file:///C:/test") }).get();

        // Resolved to WindowsOnly from [net.file].path_style -- "/C:/test"
        // must have been converted to "C:\test" before reaching the
        // override.
        DSBOX_NET_FILE_CHECK_EQ(observedPath, "C:\\test");

        module.onStop(context);
    }
}

void runFileModuleTests()
{
    runTest("testNameAndDependencies", testNameAndDependencies);
    runTest("testOnInitThrowsWithoutNetworkManagerInContext", testOnInitThrowsWithoutNetworkManagerInContext);
    runTest("testOnInitRegistersFileSchemeOnNetworkManager", testOnInitRegistersFileSchemeOnNetworkManager);
    runTest("testOnStopUnregistersFileScheme", testOnStopUnregistersFileScheme);
    runTest("testExplicitPathStyleBypassesConfig", testExplicitPathStyleBypassesConfig);
    runTest("testDefaultPathStyleFallsBackToPosixWithoutConfig", testDefaultPathStyleFallsBackToPosixWithoutConfig);
    runTest("testDefaultPathStyleIsReadFromConfig", testDefaultPathStyleIsReadFromConfig);
}
