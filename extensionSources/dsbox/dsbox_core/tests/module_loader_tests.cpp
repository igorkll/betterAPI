#include "test_framework.hpp"
#include "fixtures/test_plugin_types.hpp"

#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace
{
    void testLoadDynamicRunsFullLifecycle()
    {
        dsbox::Context context;

        auto counters = std::make_shared<dsbox_test_fixtures::LifecycleCounters>();
        context.provide(counters);

        dsbox::ModuleManager modules(context);

        auto& loaded = modules.loadDynamic(DSBOX_TEST_FIXTURE_PATH);

        DSBOX_CHECK(loaded.name() == std::string_view("dsbox.tests.fixture"));
        DSBOX_CHECK(modules.has("dsbox.tests.fixture"));

        modules.startAll();

        DSBOX_CHECK_EQ(counters->initCount.load(), 1);
        DSBOX_CHECK_EQ(counters->startCount.load(), 1);
        DSBOX_CHECK_EQ(counters->stopCount.load(), 0);
        DSBOX_CHECK_EQ(counters->shutdownCount.load(), 0);

        modules.stopAll();

        DSBOX_CHECK_EQ(counters->stopCount.load(), 1);
        DSBOX_CHECK_EQ(counters->shutdownCount.load(), 1);
    }

    void testUnloadRunsStopAndShutdownOnce()
    {
        dsbox::Context context;

        auto counters = std::make_shared<dsbox_test_fixtures::LifecycleCounters>();
        context.provide(counters);

        dsbox::ModuleManager modules(context);
        modules.loadDynamic(DSBOX_TEST_FIXTURE_PATH);
        modules.startAll();

        modules.unload("dsbox.tests.fixture");

        DSBOX_CHECK(!modules.has("dsbox.tests.fixture"));
        DSBOX_CHECK_EQ(counters->stopCount.load(), 1);
        DSBOX_CHECK_EQ(counters->shutdownCount.load(), 1);

        // Nothing further to assert directly here beyond "this didn't
        // crash" -- the library was actually unloaded (dlclose/
        // FreeLibrary) as part of unload(), so a use-after-free during
        // teardown would show up as a crash here, or under ASan/UBSan.
    }

    void testMissingLibraryThrows()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        bool thrown = false;
        try
        {
            modules.loadDynamic("this/path/does/not/exist.so");
        }
        catch (const std::exception&)
        {
            thrown = true;
        }

        DSBOX_CHECK(thrown);
    }

    void testIncompatibleVersionRejectedBeforeFactory()
    {
        dsbox::Context context;
        dsbox::ModuleManager modules(context);

        bool thrown = false;
        std::string message;

        try
        {
            modules.loadDynamic(DSBOX_TEST_FIXTURE_INCOMPATIBLE_PATH);
        }
        catch (const std::exception& error)
        {
            thrown = true;
            message = error.what();
        }

        DSBOX_CHECK(thrown);
        // If the version check didn't short-circuit before
        // DSBox_CreateModule(), the module would have registered itself
        // despite being "rejected" -- has() must be false either way.
        DSBOX_CHECK(!modules.has("dsbox.tests.incompatible"));
        DSBOX_CHECK(message.find("999") != std::string::npos);
    }
}

void runModuleLoaderTests()
{
    using namespace dsbox_tests;

    runTest("LoadDynamicRunsFullLifecycle", testLoadDynamicRunsFullLifecycle);
    runTest("UnloadRunsStopAndShutdownOnce", testUnloadRunsStopAndShutdownOnce);
    runTest("MissingLibraryThrows", testMissingLibraryThrows);
    runTest("IncompatibleVersionRejectedBeforeFactory", testIncompatibleVersionRejectedBeforeFactory);
}
