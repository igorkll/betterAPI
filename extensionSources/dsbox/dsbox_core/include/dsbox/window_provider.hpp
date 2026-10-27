#pragma once

#include <dsbox/api.hpp>

#include <cstdint>
#include <functional>

namespace dsbox
{
    // Opaque handle to a native window, owned by whichever module
    // implements IWindowProvider (e.g. dsbox_sdl::WindowModule). Lives
    // in core -- rather than in the windowing module that happens to
    // implement the contract -- so that ANY module can refer to "a
    // window" in its own public API without depending on a concrete
    // windowing backend. A backend module is free to keep its own alias
    // of this type for source compatibility (see
    // dsbox_sdl::WindowHandle).
    using WindowHandle = std::uint64_t;
    constexpr WindowHandle kInvalidWindowHandle = 0;

    // Contract for anything that owns native windows and can expose them,
    // for the duration of a callback, to another module that needs to do
    // native/graphics-API work against one (e.g. a renderer module
    // creating a swapchain surface for a window).
    //
    // This is the core-owned half of a pattern used across this
    // codebase: a windowing module (dsbox_sdl::WindowModule today,
    // potentially others later) implements IWindowProvider; any module
    // that needs "the native window behind this handle" -- without
    // needing to know or care which windowing backend produced it --
    // depends only on this interface, not on a concrete windowing
    // module. That is what lets e.g. dsbox_vulkan stay decoupled from
    // dsbox_sdl in its own public API: dsbox_vulkan::VulkanModule::
    // attachToWindow() takes an IWindowProvider&, not a
    // dsbox_sdl::WindowModule&.
    //
    // The native pointer handed to `fn` is deliberately typed as void*
    // here -- core has no idea what "native" means for any given
    // backend (HWND, SDL_Window*, a browser-hosted surface, ...). The
    // implementer and the caller agree out of band on what to
    // static_cast it back to; that agreement is exactly the "narrow,
    // deliberate coupling" a call site like dsbox_vulkan's already
    // documents, just expressed through a shared contract instead of a
    // hard dependency on one specific windowing module's header.
    class DSBOX_API IWindowProvider
    {
    public:
        virtual ~IWindowProvider() = default;

        // Runs `fn` with the native window pointer/handle behind
        // `handle`, on whatever thread the implementation requires such
        // access to happen on (implementations that don't have such a
        // restriction are free to just call `fn` inline). A no-op (fn
        // never called) if `handle` is unknown to this provider. Safe to
        // call from any thread; blocks the caller until `fn` has
        // actually run.
        virtual void withNativeWindow(WindowHandle handle, std::function<void(void*)> fn) = 0;
    };
}
