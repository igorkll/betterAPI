#pragma once

#include <dsbox/window_provider.hpp>

namespace dsbox
{
    // Contract for a renderer that wants to be driven directly by a
    // window-owning module's own control thread -- the same thread
    // that pumps native window events and feeds dsbox::Input -- rather
    // than running its own separate thread and having to hand frames
    // across some synchronization boundary. See
    // dsbox_sdl::WindowModule::setRenderHook() for how one gets
    // registered; dsbox_vulkan_gui::VulkanUiWindowDriver is (so far)
    // the one implementation.
    //
    // Every call happens SYNCHRONOUSLY on that control thread. There is
    // deliberately no "renderer's own thread" anymore for a window with
    // a hook registered: onFrame() IS this window's render loop, called
    // once per control-thread loop iteration. This is a deliberate
    // trade-off, not an oversight: it removes the resize-response lag
    // that came from rendering and event-pumping being on different
    // threads, at the cost of everything on that control thread now
    // sharing onFrame()'s own latency -- a slow frame (or a present
    // mode that blocks until vblank -- see
    // dsbox_vulkan::VulkanOptions::presentMode) delays THIS window's
    // own event processing by exactly that long, and, since one
    // WindowModule's control thread already serves every window
    // created through it, every OTHER hooked window's onFrame() call
    // that same iteration, and any pending createWindow()/
    // closeWindow()/setWindowTitle()/setRenderHook() call for ANY
    // window on that same module, too.
    class IWindowRenderHook
    {
    public:
        virtual ~IWindowRenderHook() = default;

        // Called once per control-thread loop iteration, after any
        // native/queued events pending for this window have already
        // been processed -- so a resize that just happened is already
        // reflected in `width`/`height` by the time this call sees it,
        // without needing a separate resize subscription. `width`/
        // `height` are this window's current pixel size, queried fresh
        // every call (0 if e.g. currently minimized -- a well-behaved
        // implementation should just return without attempting to
        // render in that case, rather than treating it as an error).
        //
        // Expected to run one full acquire/draw/present cycle (or
        // decide to skip this iteration). See the class comment above
        // for why a blocking present is an accepted consequence here,
        // not a bug to work around.
        virtual void onFrame(WindowHandle handle, int width, int height) = 0;
    };
}
