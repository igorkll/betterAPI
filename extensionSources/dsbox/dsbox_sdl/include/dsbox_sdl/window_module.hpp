#pragma once

#include <dsbox_sdl/api.hpp>

#include <dsbox/input.hpp>
#include <dsbox/module.hpp>
#include <dsbox/text_input_provider.hpp>
#include <dsbox/window_provider.hpp>
#include <dsbox/window_render_hook.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Forward-declared, not included: application code never touches SDL
// types directly (see WindowHandle), and the .cpp is the only place
// that needs <SDL3/SDL.h>. In C++ a tag declaration like this makes the
// bare name usable as a type on its own, matching how these are declared
// in SDL3's own headers (`typedef struct SDL_Window SDL_Window;` and
// `typedef union SDL_Event { ... } SDL_Event;`).
struct SDL_Window;
union SDL_Event;

namespace dsbox_sdl
{
    // Opaque handle to a window. Deliberately not a raw SDL_Window*:
    // SDL requires window/video operations to happen on the thread that
    // initialized SDL_INIT_VIDEO -- WindowModule's own control thread,
    // never the caller's -- so handing out real pointers would make it
    // trivially easy to call SDL from the wrong thread by accident.
    // Every operation on a window goes through WindowModule's methods
    // instead, which route the actual SDL call to the control thread.
    //
    // Aliased onto dsbox::WindowHandle/kInvalidWindowHandle (see
    // <dsbox/window_provider.hpp>) rather than being its own distinct
    // type: WindowModule implements dsbox::IWindowProvider, and a
    // handle has to mean the same thing on both sides of that contract
    // for a caller holding one from createWindow() to be able to hand
    // it to an IWindowProvider-consuming module (e.g. dsbox_vulkan)
    // without a conversion. Existing code that spells it
    // dsbox_sdl::WindowHandle keeps compiling unchanged.
    using WindowHandle = dsbox::WindowHandle;
    constexpr WindowHandle kInvalidWindowHandle = dsbox::kInvalidWindowHandle;

    // The keyboard/mouse vocabulary and event payload types below used
    // to be defined here, but they never actually had any SDL-specific
    // content -- only SDL-derived numeric values (see the long comment
    // on dsbox::KeyCode). They now live in <dsbox/input.hpp>, alongside
    // dsbox::Input (see WindowOptions and inputFor() below for how this
    // module uses it). These aliases keep existing code that spells
    // dsbox_sdl::KeyCode, dsbox_sdl::KeyEvent, etc. compiling unchanged.
    using KeyCode = dsbox::KeyCode;
    using KeyModifier = dsbox::KeyModifier;
    using MouseButton = dsbox::MouseButton;
    using KeyEvent = dsbox::KeyEvent;
    using MouseButtonEvent = dsbox::MouseButtonEvent;
    using MouseMotionEvent = dsbox::MouseMotionEvent;
    using MouseWheelEvent = dsbox::MouseWheelEvent;

    // Which native windowing-system flag to request at creation time,
    // for renderer mechanisms that require the window to have been
    // created with a specific flag up front (SDL enforces this only at
    // creation -- it cannot be added to an existing window later, so
    // this can't just be a property a renderer module flips when it
    // attaches). WindowModule has no idea what Vulkan/OpenGL/etc. even
    // are; this is purely "which bit to pass to the underlying window
    // creation call," left as a hint precisely so new mechanisms can be
    // added here later without WindowModule depending on them.
    enum class WindowGraphicsHint
    {
        // No special flag. Suitable for windows with no renderer at
        // all, or one that doesn't require a creation-time flag (e.g.
        // SDL_Renderer). Default.
        None,

        // Requests SDL_WINDOW_VULKAN. Required before a Vulkan surface
        // (e.g. via dsbox_vulkan) can be created for this window.
        Vulkan,

        // Requests SDL_WINDOW_OPENGL. Not used by dsbox_vulkan; reserved
        // for a possible future OpenGL-based renderer module.
        OpenGL,
    };

    struct WindowOptions
    {
        std::string title = "DSBox Window";
        int width = 800;
        int height = 600;
        bool resizable = true;

        // See WindowGraphicsHint. Set this before attaching a renderer
        // that needs it (e.g. WindowGraphicsHint::Vulkan for
        // dsbox_vulkan) -- it cannot be changed on an existing window.
        WindowGraphicsHint graphicsHint = WindowGraphicsHint::None;

        // If true (the default), the control thread destroys the window
        // itself the moment the user requests a close (e.g. clicks the
        // titlebar close button) and publishes "window.closed" on the
        // bus. If false, a close request instead publishes
        // "window.close_requested" WITHOUT destroying anything -- the
        // application decides (e.g. to prompt "save changes?") and must
        // call closeWindow() itself when ready.
        bool autoHandleCloseEvent = true;

        // Master switch for this window's keyboard/mouse input. If
        // false, the control thread does not feed this window's SDL
        // input events into its dsbox::Input at all, regardless of the
        // two flags below. Default true.
        bool inputEnabled = true;

        // Forwarded into this window's dsbox::Input as
        // InputOptions::dispatchEvents -- see <dsbox/input.hpp>. If
        // true (and inputEnabled), publishes "window.key",
        // "window.mouse_button", "window.mouse_motion", and
        // "window.mouse_wheel" on the bus as input arrives. Default
        // true.
        bool dispatchInputEvents = true;

        // Forwarded into this window's dsbox::Input as
        // InputOptions::trackState. If true (and inputEnabled), that
        // Input also maintains queryable state, reachable via
        // WindowModule::isKeyDown()/isMouseButtonDown()/mousePosition()
        // or directly via inputFor(handle). Default false: most windows
        // don't need the extra bookkeeping.
        bool trackInputState = false;
    };

    struct WindowResizedEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        int width = 0;
        int height = 0;
    };

    struct WindowFocusEvent
    {
        WindowHandle handle = kInvalidWindowHandle;
        bool focused = false;
    };

    // Configuration for a single WindowModule instance, fixed for its
    // whole lifetime. Per-window configuration is WindowOptions, passed
    // to createWindow() -- this is only for the module/control-thread
    // level.
    struct WindowModuleOptions
    {
        // Maximum number of not-yet-processed commands (from
        // createWindow(), closeWindow(), setWindowTitle(),
        // withNativeWindow()) post() will let build up on the control
        // thread's queue before it starts blocking the calling thread
        // until the control thread catches up. 0 (the default) means
        // unbounded -- matches this class's original behavior. In
        // practice most of these calls already block their caller on a
        // future waiting for the command to run, so this mainly bounds
        // setWindowTitle()'s fire-and-forget queue.
        std::size_t maxCommandQueueSize = 0;
    };

    // Owns a dedicated "control thread" that initializes SDL, pumps its
    // event queue, and is the only thread that ever touches an
    // SDL_Window. Other threads interact with it by posting work onto a
    // queue (create/close/rename a window); the control thread wakes
    // immediately when work is posted (via a registered custom SDL
    // event pushed onto SDL's own queue, not polling), interleaving
    // that work with normal event handling.
    //
    // v1 scope was window lifecycle only (create, close, resize, focus
    // change); keyboard/mouse input routing was added afterward, gated
    // per-window by WindowOptions::inputEnabled/dispatchInputEvents/
    // trackInputState. Discrete key up/down goes through dsbox::Input
    // as before. Text input (Unicode/IME) is reported separately, via
    // this class's dsbox::ITextInputProvider implementation: SDL only
    // generates text events for a window between startTextInput() and
    // stopTextInput(), so a UI layer (dsbox_ui) turns it on while a
    // text-accepting element has focus. While active, this module
    // publishes "window.text_input" (committed text) and
    // "window.text_editing" (IME preview) on the bus -- see
    // <dsbox/text_input_provider.hpp>. Both honor
    // WindowOptions::inputEnabled, same as keys and mouse.
    //
    // Input: each window gets its own dsbox::Input instance (see
    // <dsbox/input.hpp>), created alongside the window in createWindow()
    // and destroyed alongside it. The control thread only ever
    // translates an SDL_Event and hands it to that window's Input via
    // setKeyState()/setMouseButtonState()/setMousePosition()/
    // setMouseWheel() -- it has no dispatch/tracking logic of its own
    // anymore. inputFor() exposes that same Input to any other caller,
    // so another module can report input for a window (emulation,
    // automation, replay, tests) through the exact same call the
    // control thread itself uses -- indistinguishable, from the
    // outside, from real input.
    //
    // Threading model: by design, this module never runs on the
    // process's actual main thread -- the host application owns that.
    // This also means true macOS/iOS support is out of scope (Cocoa
    // requires video/window operations on the real main thread); this
    // module targets Windows and Linux.
    //
    // Render hooks: setRenderHook() lets a renderer register to be
    // called once per control-thread loop iteration for a specific
    // window (see IWindowRenderHook), rather than running its own
    // separate render thread. This is what makes a resize feel
    // immediate -- the render call for a window happens right after
    // that window's resize event is processed, on the SAME thread,
    // same iteration -- at the cost of every hooked window (and every
    // pending createWindow()/closeWindow()/setWindowTitle()/
    // setRenderHook() call for ANY window on this same module) sharing
    // whatever latency that render call has. A window with no hook
    // registered behaves exactly as before: this thread only ever pumps
    // its events. See IWindowRenderHook's own comment for the full
    // trade-off before registering one.
    //
    // Lifecycle: the control thread starts in onInit() (which blocks
    // until SDL has actually finished initializing, so it's safe to use
    // this module immediately after onInit returns) and is stopped --
    // destroying any windows still open -- in onShutdown(), which blocks
    // until the thread has fully exited. The destructor performs the
    // same stop+join if onShutdown was never called.
    class DSBOX_SDL_API WindowModule final
        : public dsbox::IModule
        , public dsbox::IWindowProvider
        , public dsbox::ITextInputProvider
    {
    public:
        WindowModule();
        explicit WindowModule(WindowModuleOptions options);
        ~WindowModule() override;

        WindowModule(const WindowModule&) = delete;
        WindowModule& operator=(const WindowModule&) = delete;

        std::string_view name() const override
        {
            return "dsbox.sdl.window";
        }

        void onInit(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Safe to call from any thread. Blocks until the window has
        // actually been created (or creation failed) on the control
        // thread. Returns kInvalidWindowHandle if SDL failed to
        // initialize at all, or if window creation itself failed --
        // check lastError() for why either way.
        WindowHandle createWindow(WindowOptions options);

        // Safe to call from any thread. Blocks until the window has
        // actually been destroyed. A no-op (returns immediately) if
        // `handle` is unknown -- already closed, or never existed.
        void closeWindow(WindowHandle handle);

        // Safe to call from any thread. Fire-and-forget: queued onto the
        // control thread, does not block waiting for it to take effect.
        // See WindowModuleOptions::maxCommandQueueSize for when this can
        // block instead.
        void setWindowTitle(WindowHandle handle, std::string title);

        // The sanctioned way for a renderer mechanism -- Vulkan, SDL_GPU,
        // SDL_Renderer, something else entirely, or none at all -- to
        // attach itself to a window. WindowModule deliberately has no
        // opinion on rendering; this is the one escape hatch that lets
        // it stay that way. `fn` runs on the control thread (the only
        // thread ever allowed to touch an SDL_Window) and is handed the
        // raw SDL_Window* for exactly as long as it runs -- do whatever
        // native/graphics-API setup you need inside it (e.g. call
        // SDL_Vulkan_CreateSurface, SDL_CreateGPUDevice +
        // SDL_ClaimWindowForGPUDevice, SDL_CreateRenderer, or read a
        // platform HWND/X11/Wayland handle via SDL_GetWindowProperties)
        // and do not store the pointer anywhere it might be read after
        // this call returns.
        //
        // Different windows are free to use different (or no) renderer
        // mechanisms -- WindowModule tracks none of this; whatever
        // module calls withNativeWindow() owns tracking its own
        // per-window renderer state entirely on its own side.
        //
        // Safe to call from any thread. Blocks the caller until `fn` has
        // run. A no-op (fn is never called) if `handle` is unknown.
        void withNativeWindow(WindowHandle handle, std::function<void(SDL_Window*)> fn);

        // dsbox::IWindowProvider implementation. Same contract as the
        // SDL_Window* overload above (same underlying call, in fact --
        // this just wraps `fn` to receive the pointer as void* instead)
        // for callers that only depend on <dsbox/window_provider.hpp>
        // and have no reason to know this handle came from SDL at all
        // (e.g. dsbox_vulkan::VulkanModule::attachToWindow()). Prefer
        // the SDL_Window* overload when the caller already depends on
        // dsbox_sdl directly -- it avoids the extra cast back from void*.
        void withNativeWindow(dsbox::WindowHandle handle, std::function<void(void*)> fn) override;

        // Registers `hook` to be called once per control-thread loop
        // iteration for `handle` -- see IWindowRenderHook's own
        // contract for exactly what that means (in particular, for
        // threading/latency). Pass nullptr to clear a previously
        // registered hook.
        //
        // Safe to call from any thread. BLOCKS until the change has
        // actually taken effect on the control thread -- in
        // particular, until any onFrame() call already in progress for
        // `handle` at the time this is called has finished -- so it is
        // safe to destroy whatever object was passed as `hook`
        // immediately after a clearing call (setRenderHook(handle,
        // nullptr)) returns; no further synchronization is needed on
        // the caller's side. A no-op (still blocks, then returns) if
        // `handle` is unknown.
        void setRenderHook(WindowHandle handle, dsbox::IWindowRenderHook* hook);

        // -- dsbox::ITextInputProvider --
        //
        // All three are safe to call from any thread, including the
        // control thread itself (e.g. from inside a "window.key"
        // subscriber): they are fire-and-forget, and when already on
        // the control thread they run immediately instead of being
        // queued. No-ops for an unknown `handle`.

        // SDL_StartTextInput() for the window. Until this is called SDL
        // generates no text events for it at all.
        void startTextInput(WindowHandle handle) override;

        void stopTextInput(WindowHandle handle) override;

        // SDL_SetTextInputArea(): where the platform IME should place
        // its candidate window, in window coordinates.
        void setTextInputRect(WindowHandle handle, int x, int y, int width, int height) override;

        std::string lastError() const;

        // Test/debug only: exposes the underlying SDL window ID for
        // `handle` (0 if unknown) so tests can synthesize window events
        // targeting a specific window. Application code has no
        // legitimate use for this and should not call it -- see
        // WindowHandle's own docs for why raw SDL identifiers are
        // otherwise kept out of this API entirely.
        std::uint32_t debugGetSDLWindowId(WindowHandle handle) const;

        // The dsbox::Input instance owned by `handle`'s window, or
        // nullptr if `handle` is unknown. This is the actual mechanism
        // behind isKeyDown()/isMouseButtonDown()/mousePosition() below
        // -- exposed directly so any other module can report input for
        // this window (setKeyState() etc.) through the same call the
        // control thread itself uses, or subscribe/query without going
        // through WindowModule at all. Safe to call from any thread;
        // the returned pointer is valid for as long as `handle`'s window
        // stays open.
        dsbox::Input* inputFor(WindowHandle handle) const;

        // Thin forwarders to inputFor(handle)->isKeyDown()/
        // isMouseButtonDown()/mousePosition() -- false/default if
        // `handle` is unknown or that window's Input isn't tracking
        // state (see WindowOptions::trackInputState). Kept on
        // WindowModule for convenience; prefer inputFor() directly if
        // you're going to make several calls against the same window.
        bool isKeyDown(WindowHandle handle, KeyCode key) const;
        bool isMouseButtonDown(WindowHandle handle, MouseButton button) const;
        bool mousePosition(WindowHandle handle, int& outX, int& outY) const;

        // Whether `handle` currently refers to a live window. Safe to
        // call from any thread.
        bool isWindowValid(WindowHandle handle) const;

        // Current client-area size in pixels for `handle`, written into
        // outWidth/outHeight. Returns false (leaving both untouched) if
        // `handle` is unknown. Safe to call from any thread and does
        // NOT post to the control thread -- it reads a cache this
        // module keeps up to date as part of handling
        // SDL_EVENT_WINDOW_RESIZED (and seeds at creation), the same
        // way mousePosition() above reads dsbox::Input's own
        // already-tracked state rather than querying SDL fresh each
        // call. Intended for a renderer running on its OWN thread that
        // wants this window's latest known size once per frame without
        // any "window.resized" subscription of its own -- see
        // dsbox_vulkan_gui::VulkanUiWindowDriver for the intended usage.
        bool windowSize(WindowHandle handle, int& outWidth, int& outHeight) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
