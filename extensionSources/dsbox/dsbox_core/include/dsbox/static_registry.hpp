#pragma once

#include <dsbox/api.hpp>
#include <dsbox/module.hpp>
#include <dsbox/plugin_export.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace dsbox
{
    using ModuleFactory = std::function<std::unique_ptr<IModule>()>;

    // Global registry of module factories contributed by DSBOX_STATIC_MODULE
    // at static-initialization time, i.e. before main() runs. ModuleManager
    // pulls from this via registerStaticModules() instead of every module
    // needing to be listed by hand in the launcher.
    //
    // Thread safety: add() and factories() are safe to call concurrently.
    // This matters because a dynamically loaded plugin's own static
    // initializers run on whichever thread called loadDynamic() -- if
    // that happens on a background thread while another thread is
    // calling registerStaticModules(), both add() and factories() may
    // genuinely race. factories() returns a snapshot copy rather than a
    // reference so a concurrent add() can never invalidate it mid-use.
    class DSBOX_API StaticModuleRegistry
    {
    public:
        static StaticModuleRegistry& instance();

        // Returns true always; the return value only exists so the call
        // can be used as the initializer of a static bool (see the
        // DSBOX_STATIC_MODULE macro below).
        bool add(ModuleFactory factory);

        std::vector<ModuleFactory> factories() const;

    private:
        StaticModuleRegistry() = default;

        std::vector<ModuleFactory> m_factories;
    };
}

// Place this macro once in a .cpp file to make `ClassName` available to
// ModuleManager::registerStaticModules() without any manual wiring in the
// launcher. `ClassName` must be default-constructible and derive from
// dsbox::IModule. `ClassName` may be namespace-qualified (e.g.
// dsbox_console::ConsoleModule).
//
//   class ConsoleModule final : public dsbox::IModule { ... };
//   DSBOX_STATIC_MODULE(ConsoleModule)
#define DSBOX_STATIC_MODULE_CONCAT_IMPL(a, b) a##b
#define DSBOX_STATIC_MODULE_CONCAT(a, b) DSBOX_STATIC_MODULE_CONCAT_IMPL(a, b)

#define DSBOX_STATIC_MODULE(ClassName)                                       \
    namespace                                                                \
    {                                                                        \
        const bool DSBOX_STATIC_MODULE_CONCAT(dsbox_static_module_registered_, __LINE__) = \
            ::dsbox::StaticModuleRegistry::instance().add(                   \
                []() -> std::unique_ptr<::dsbox::IModule>                    \
                {                                                            \
                    return std::make_unique<ClassName>();                    \
                });                                                          \
    }

// Use for a module built as its own shared library. It preserves automatic
// static registration and also provides the C ABI ModuleManager resolves
// when loading the library dynamically.
#define DSBOX_DYNAMIC_MODULE(ClassName)                                      \
    DSBOX_STATIC_MODULE(ClassName)                                           \
    DSBOX_PLUGIN_EXPORT ::dsbox::IModule* DSBox_CreateModule()               \
    {                                                                        \
        return new ClassName();                                              \
    }                                                                        \
    DSBOX_PLUGIN_EXPORT void DSBox_DestroyModule(::dsbox::IModule* module)   \
    {                                                                        \
        delete module;                                                       \
    }
