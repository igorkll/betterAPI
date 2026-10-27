// dsbox_adb_tool -- opens a window for driving dsbox_android_adb against
// a real phone (device list, shell/install/uninstall/push/pull/reboot,
// package listing, live logcat). The tool is GUI-only; command input is
// provided by the GUI panels.
//
// The GUI's own panels (one per ADB operation) are built through
// dsbox_ui::PanelRegistry (published into Context here, before any
// static or dynamically loaded module starts) -- see gui_mode.cpp and
// dsbox_ui/panel.hpp's own class comment. Any OTHER module, statically
// linked into this binary or loaded later via
// dsbox::ModuleManager::loadDynamic(), can register its own panel from
// its own onStart() the exact same way, with no dependency on this
// file, on dsbox_sdl/dsbox_vulkan/dsbox_vulkan_gui, or on having
// existed when the window was first built.
//
// Split across several .cpp files (launch_options/module_config/
// adb_format/gui_mode) purely to keep each one a reasonable
// size -- no behavioral seam beyond what their names say.

#include <dsbox/bootstrap_paths.hpp>
#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>
#include <dsbox/version.hpp>

#include <dsbox_config/module_config_loader.hpp>
#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox_android_adb/adb_module.hpp>

#include "gui_mode.hpp"
#include "launch_options.hpp"
#include "module_config.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using dsbox_android_adb::AdbModule;

int main(int argc, char* argv[])
{
    // Must happen before ANY reference to a delay-loaded DSBox:: DLL
    // (see cmake/DSBoxDelayLoad.cmake) -- including the dsbox::version*()
    // calls the banner below makes into DSBoxCore.dll. Widens the
    // process's DLL search path to cover every bin/framework/<subsystem>/
    // subfolder; a no-op on non-Windows, where RPATH already handles
    // this at link time. Uses dsbox_adb_tool::executableDirectory()
    // (not options.baseDir, built further down) since --pathmode can
    // point baseDir at config/modules somewhere else entirely while the
    // DLLs themselves always stay next to this binary.
    // bootstrapFrameworkSearchPaths() takes the FRAMEWORK ROOT
    // (fin-out/bin/framework/), not this executable's own directory --
    // see bootstrap_paths.hpp's own top comment.
    dsbox::bootstrapFrameworkSearchPaths(
        (dsbox_adb_tool::executableDirectory() / ".." / "framework").string());
    dsbox::preloadFrameworkModule("DSBoxAndroidAdb.dll");
    dsbox::preloadFrameworkModule("DSBoxSDL.dll");
    dsbox::preloadFrameworkModule("DSBoxVulkan.dll");

    std::printf(
        "DSBox ADB Tool -- built on DSBox %u.%u.%u (%s)\n\n",
        dsbox::versionMajor(),
        dsbox::versionMinor(),
        dsbox::versionPatch(),
        dsbox::versionString()
    );

    dsbox_adb_tool::LaunchOptions options;

    try
    {
        options = dsbox_adb_tool::parseArgs(argc, argv);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }

    std::printf("Base directory: %s\n", options.baseDir.string().c_str());

    // configDir comes from LaunchOptions, not baseDir / "config" --
    // under the default --pathmode 0 it points at the repo's
    // consolidated top-level config/dsbox_adb_tool/ tree instead (see
    // LaunchOptions::configDir's own doc comment). modulesDir is
    // unaffected: plugin .dll/.so files loaded via modules.ini are a
    // separate concern from configuration and keep their existing
    // baseDir-relative layout.
    std::filesystem::path configDir = options.configDir;
    std::filesystem::path modulesDir = options.baseDir / "modules";

    std::filesystem::path defaultsIniPath = configDir / "default" / "config.ini";
    std::filesystem::path overridesIniPath = configDir / "config.ini";

    std::shared_ptr<dsbox_config::PersistentConfigStore> configStore;

    try
    {
        configStore = std::make_shared<dsbox_config::PersistentConfigStore>(
            dsbox_config::PersistentConfigStore::load(
                dsbox_adb_tool::readFileOrEmpty(defaultsIniPath),
                dsbox_adb_tool::readFileOrEmpty(overridesIniPath)));
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "dsbox_adb_tool: failed to load config: %s\n", error.what());
        return EXIT_FAILURE;
    }

    dsbox::Context context;
    dsbox::ModuleManager modules(context);

    context.provide(configStore);

    // Pulls in every module registered via DSBOX_STATIC_MODULE in this
    // binary: dsbox_android_adb::AdbModule (with dsbox_scheduler::SchedulerModule
    // linked in transitively through it), and -- now that this target
    // also links DSBox::SDL/DSBox::Vulkan for the GUI path --
    // dsbox_sdl::WindowModule and dsbox_vulkan::VulkanModule too.
    // dsbox_ui::UiModule deliberately does NOT self-register this way
    // (see its own CMakeLists.txt comment); runGui() registers it by
    // hand.
    modules.registerStaticModules();

    std::filesystem::path defaultModulesIni = configDir / "modules.ini";

    if (std::filesystem::exists(defaultModulesIni))
    {
        if (!dsbox_adb_tool::loadModuleConfig(modules, defaultModulesIni, modulesDir))
            return EXIT_FAILURE;
    }

    for (const auto& arg : options.extraArgs)
    {
        if (dsbox_adb_tool::endsWith(arg, ".ini"))
        {
            std::filesystem::path path(arg);

            if (!dsbox_adb_tool::loadModuleConfig(modules, path, path.parent_path()))
                return EXIT_FAILURE;

            continue;
        }

        try
        {
            auto& loaded = modules.loadDynamic(arg);
            std::printf("Loaded dynamic module '%.*s' from %s\n",
                static_cast<int>(loaded.name().size()), loaded.name().data(), arg.c_str());
        }
        catch (const std::exception& exception)
        {
            std::fprintf(stderr, "Failed to load '%s': %s\n", arg.c_str(), exception.what());
            return EXIT_FAILURE;
        }
    }

    try
    {
        modules.startAll();
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Failed to start modules: %s\n", error.what());
        return EXIT_FAILURE;
    }
    catch (...)
    {
        std::fprintf(stderr, "Failed to start modules: unknown (non-std::exception) error\n");
        return EXIT_FAILURE;
    }

    auto* adb = dynamic_cast<AdbModule*>(modules.find("dsbox.android.adb"));

    if (!adb)
    {
        std::fprintf(stderr, "dsbox_adb_tool: required modules did not start\n");
        modules.stopAll();
        return EXIT_FAILURE;
    }

    dsbox_adb_tool::runGui(context, modules, *adb);

    modules.stopAll();

    if (configStore->isDirty())
    {
        std::string overridesText = configStore->serializeOverrides();

        std::error_code ec;
        std::filesystem::create_directories(overridesIniPath.parent_path(), ec);

        std::ofstream out(overridesIniPath, std::ios::binary | std::ios::trunc);

        if (out)
        {
            out << overridesText;
            configStore->markSaved();
            std::printf("Saved config to %s\n", overridesIniPath.string().c_str());
        }
        else
        {
            std::fprintf(
                stderr, "Failed to write config to %s\n", overridesIniPath.string().c_str());
        }
    }

    return EXIT_SUCCESS;
}
