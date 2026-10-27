// A plugin that deliberately declares an unsatisfiable core version
// requirement, to exercise ModuleManager::loadDynamic()'s rejection path.
// DSBox_CreateModule() below must never be called for this plugin -- the
// version check has to happen first.
#include <dsbox/context.hpp>
#include <dsbox/module.hpp>
#include <dsbox/plugin_export.hpp>
#include <dsbox/plugin_version.hpp>

namespace
{
    class NeverLoadedModule final : public dsbox::IModule
    {
    public:
        std::string_view name() const override
        {
            return "dsbox.tests.incompatible";
        }
    };
}

DSBOX_PLUGIN_EXPORT dsbox::IModule* DSBox_CreateModule()
{
    return new NeverLoadedModule();
}

DSBOX_PLUGIN_EXPORT void DSBox_DestroyModule(dsbox::IModule* module)
{
    delete module;
}

// No realistic host will ever be core version 999.0.0.
DSBOX_REQUIRE_CORE_VERSION_EXACT(999, 0, 0)
