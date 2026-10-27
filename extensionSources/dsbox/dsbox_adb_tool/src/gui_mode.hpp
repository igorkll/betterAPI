#pragma once

namespace dsbox
{
    class Context;
    class ModuleManager;
}

namespace dsbox_android_adb
{
    class AdbModule;
}

namespace dsbox_adb_tool
{
    // Opens a window hosting a sidebar of switchable panels (built
    // through dsbox_ui::PanelRegistry, published into `context` here)
    // and blocks until every such window is closed. This is the
    // only supported mode.
    //
    // Each window carries its own selected device (picked from the
    // Devices panel, which the built-in panels now act as -- Shell/
    // Packages/Logcat all read that window's selection instead of each
    // keeping its own typed-in serial). Packages merges what used to
    // be two separate panels ("Packages" and "Install / Uninstall"):
    // Install now lives at the top of the package list, and clicking a
    // package opens a detail view (Uninstall/Clear Data/Force Stop/
    // info) in place of the list, inside the same panel. The Devices panel's
    // "Open in new window" button opens an additional, independent
    // window -- same panel set, its own device selection, its own
    // render loop on its own thread -- so several devices can be
    // driven side by side. Opening extra windows is entirely optional;
    // a person who never clicks that button sees exactly the single-
    // window tool this always was.
    //
    // Registers a dsbox_sdl::WindowModule, a dsbox_vulkan::VulkanModule
    // and a dsbox_ui::UiModule into `modules` itself (the first two
    // self-register via DSBOX_STATIC_MODULE once dsbox_adb_tool links
    // against DSBox::SDL/DSBox::Vulkan -- see registerStaticModules()'s
    // own docs -- but dsbox_ui::UiModule deliberately does not, so this
    // function registers it by hand) and initializes dsbox_ui::UiModule
    // by hand (not through modules.startAll(), which would re-run
    // onInit() on every already-started module -- see this file's own
    // implementation comments) before opening the first window.
    //
    // Returns once every window this call opened has closed; the
    // caller is still responsible for calling modules.stopAll()
    // afterwards.
    void runGui(dsbox::Context& context, dsbox::ModuleManager& modules,
                dsbox_android_adb::AdbModule& adb);
}
