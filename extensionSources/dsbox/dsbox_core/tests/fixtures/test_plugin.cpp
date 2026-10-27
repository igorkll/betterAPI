// A minimal real plugin, built as an actual .dll/.so by CMake and loaded
// by module_loader_tests.cpp through ModuleManager::loadDynamic() --
// this is what gives that code path automated regression coverage
// instead of only having been checked by hand.
#include "test_plugin_types.hpp"

#include <dsbox/context.hpp>
#include <dsbox/module.hpp>
#include <dsbox/plugin_export.hpp>

namespace
{
    class TestFixtureModule final : public dsbox::IModule
    {
    public:
        std::string_view name() const override
        {
            return "dsbox.tests.fixture";
        }

        void onInit(dsbox::Context& context) override
        {
            if (auto counters = context.get<dsbox_test_fixtures::LifecycleCounters>())
                counters->initCount++;
        }

        void onStart(dsbox::Context& context) override
        {
            if (auto counters = context.get<dsbox_test_fixtures::LifecycleCounters>())
                counters->startCount++;
        }

        void onStop(dsbox::Context& context) override
        {
            if (auto counters = context.get<dsbox_test_fixtures::LifecycleCounters>())
                counters->stopCount++;
        }

        void onShutdown(dsbox::Context& context) override
        {
            if (auto counters = context.get<dsbox_test_fixtures::LifecycleCounters>())
                counters->shutdownCount++;
        }
    };
}

DSBOX_PLUGIN_EXPORT dsbox::IModule* DSBox_CreateModule()
{
    return new TestFixtureModule();
}

DSBOX_PLUGIN_EXPORT void DSBox_DestroyModule(dsbox::IModule* module)
{
    delete module;
}
