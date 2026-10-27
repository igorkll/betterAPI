#include <dsbox_sdl/window_module.hpp>

#include <dsbox/plugin_export.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // Translates SDL's modifier bitmask into our own KeyModifier bitmask.
    // Deliberately collapses left/right variants (SDL_KMOD_LSHIFT vs.
    // SDL_KMOD_RSHIFT, etc.) into one flag each -- KeyEvent::key already
    // distinguishes LeftShift from RightShift for callers who care which
    // side was pressed; modifiers only need to answer "is shift held".
    dsbox_sdl::KeyModifier translateModifiers(SDL_Keymod mod)
    {
        dsbox_sdl::KeyModifier result = dsbox_sdl::KeyModifier::None;

        if (mod & SDL_KMOD_SHIFT)
            result = result | dsbox_sdl::KeyModifier::Shift;
        if (mod & SDL_KMOD_CTRL)
            result = result | dsbox_sdl::KeyModifier::Control;
        if (mod & SDL_KMOD_ALT)
            result = result | dsbox_sdl::KeyModifier::Alt;
        if (mod & SDL_KMOD_GUI)
            result = result | dsbox_sdl::KeyModifier::Super;
        if (mod & SDL_KMOD_CAPS)
            result = result | dsbox_sdl::KeyModifier::CapsLock;
        if (mod & SDL_KMOD_NUM)
            result = result | dsbox_sdl::KeyModifier::NumLock;

        return result;
    }

    // Translates a physical SDL_Scancode (layout-independent) into our
    // own KeyCode. This is a straight numeric cast, not a hand-maintained
    // table -- see the long comment on dsbox::KeyCode's definition in
    // <dsbox/input.hpp> for why that's safe: every KeyCode value is
    // pinned to SDL's own scancode number. The static_asserts below are
    // a cheap smoke test that this hasn't silently drifted (e.g. from a
    // stale SDL version), not an exhaustive check.
    dsbox_sdl::KeyCode translateScancode(SDL_Scancode scancode)
    {
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::Unknown) == SDL_SCANCODE_UNKNOWN);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::A) == SDL_SCANCODE_A);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::Num0) == SDL_SCANCODE_0);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::Left) == SDL_SCANCODE_LEFT);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::LeftControl) == SDL_SCANCODE_LCTRL);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::RightSuper) == SDL_SCANCODE_RGUI);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::F12) == SDL_SCANCODE_F12);
        static_assert(static_cast<SDL_Scancode>(dsbox_sdl::KeyCode::EndCall) == SDL_SCANCODE_ENDCALL);

        return static_cast<dsbox_sdl::KeyCode>(scancode);
    }

    // Same idea as translateScancode(): SDL_Button* values are pinned
    // 1:1 onto MouseButton, so this is a cast plus a smoke test rather
    // than a table.
    dsbox_sdl::MouseButton translateMouseButton(Uint8 button)
    {
        static_assert(static_cast<Uint8>(dsbox_sdl::MouseButton::Left) == SDL_BUTTON_LEFT);
        static_assert(static_cast<Uint8>(dsbox_sdl::MouseButton::Middle) == SDL_BUTTON_MIDDLE);
        static_assert(static_cast<Uint8>(dsbox_sdl::MouseButton::Right) == SDL_BUTTON_RIGHT);
        static_assert(static_cast<Uint8>(dsbox_sdl::MouseButton::X1) == SDL_BUTTON_X1);
        static_assert(static_cast<Uint8>(dsbox_sdl::MouseButton::X2) == SDL_BUTTON_X2);

        return static_cast<dsbox_sdl::MouseButton>(button);
    }
}

namespace dsbox_sdl
{
    struct WindowModule::Impl
    {
        explicit Impl(WindowModuleOptions opts) : options(std::move(opts)) {}

        struct WindowConfig
        {
            bool autoHandleCloseEvent = true;
            bool inputEnabled = true;
        };

        WindowModuleOptions options;

        dsbox::Context* context = nullptr;

        std::thread controlThread;
        std::thread::id controlThreadId; // set once at the top of controlThreadLoop(), before readyPromise fires -- see runOrPost()
        std::atomic<bool> running{ false };
        std::atomic<bool> initialized{ false };
        std::uint32_t wakeEventType = 0;

        std::mutex commandMutex;
        std::condition_variable spaceAvailable; // wakes post() when maxCommandQueueSize is bounded and full
        std::deque<std::function<void()>> commands;

        mutable std::mutex windowsMutex;
        std::unordered_map<WindowHandle, SDL_Window*> windows;
        std::unordered_map<WindowHandle, WindowConfig> windowConfigs;
        std::unordered_map<WindowHandle, std::uint32_t> windowIds;
        std::unordered_map<WindowHandle, std::unique_ptr<dsbox::Input>> inputs;
        std::unordered_map<WindowHandle, std::pair<int, int>> windowSizes; // seeded at creation, kept current on SDL_EVENT_WINDOW_RESIZED -- see WindowModule::windowSize()
        std::atomic<std::uint64_t> nextHandle{ 1 };

        // Only ever read or written on the control thread itself: every
        // mutation goes through setRenderHook(), which posts a command
        // and blocks until the control thread has actually run it (see
        // that method's own doc), and runRenderHooks() -- the only
        // reader -- runs on the control thread too. No separate mutex
        // needed as a result, unlike `windows`/`inputs` above, which
        // ARE read directly from other threads.
        std::unordered_map<WindowHandle, dsbox::IWindowRenderHook*> renderHooks;

        // Count of mouse buttons currently held down, across every
        // window this module owns. Only ever touched on the control
        // thread (handleSDLEvent runs there), so no separate mutex is
        // needed. Drives SDL_CaptureMouse(): without it, SDL stops
        // delivering mouse motion/button-up events to a window once the
        // cursor leaves its bounds, so a press-drag-release-outside
        // sequence would never produce a button-up event and
        // dsbox::Input would report that button as stuck down forever.
        // Capturing while at least one button is held keeps delivering
        // those events (with coordinates that may fall outside the
        // window) until every button has actually been released.
        int heldMouseButtonCount = 0;

        mutable std::mutex errorMutex;
        std::string lastError;

        void controlThreadLoop(std::promise<void> readyPromise);
        void processCommands();
        void handleSDLEvent(const SDL_Event& event);
        void post(std::function<void()> command);
        void runOrPost(std::function<void()> command);
        bool destroyWindowInternal(WindowHandle handle);
        void runRenderHooks();
        void setError(std::string message);
        WindowHandle handleForWindow(SDL_Window* window) const;
        WindowConfig configFor(WindowHandle handle) const;
    };

    void WindowModule::Impl::setError(std::string message)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = std::move(message);
    }

    void WindowModule::Impl::post(std::function<void()> command)
    {
        {
            std::unique_lock<std::mutex> lock(commandMutex);

            if (options.maxCommandQueueSize > 0)
            {
                spaceAvailable.wait(lock, [this]
                {
                    return !running.load() || commands.size() < options.maxCommandQueueSize;
                });
            }

            commands.push_back(std::move(command));
        }

        SDL_Event wakeEvent{};
        wakeEvent.type = wakeEventType;
        SDL_PushEvent(&wakeEvent);
    }

    // Runs `command` immediately if already on the control thread,
    // otherwise queues it exactly like post(). Exists for the
    // ITextInputProvider calls: a UI layer invokes them from inside its
    // own event handling, which itself runs ON the control thread (it's
    // driven by "window.key"/"window.mouse_button" bus events emitted
    // from handleSDLEvent()) -- queueing there would delay the SDL call
    // to a later loop iteration for no benefit, and with a bounded
    // command queue could even block the control thread on itself.
    void WindowModule::Impl::runOrPost(std::function<void()> command)
    {
        if (std::this_thread::get_id() == controlThreadId)
            command();
        else
            post(std::move(command));
    }

    void WindowModule::Impl::processCommands()
    {
        std::deque<std::function<void()>> pending;

        {
            std::lock_guard<std::mutex> lock(commandMutex);
            std::swap(pending, commands);
        }

        // A batch of slots just freed up -- wake any post() blocked on a
        // full bounded queue before running the commands themselves, so
        // producers and the control thread make progress concurrently.
        spaceAvailable.notify_all();

        for (auto& command : pending)
            command();
    }

    bool WindowModule::Impl::destroyWindowInternal(WindowHandle handle)
    {
        SDL_Window* window = nullptr;

        {
            std::lock_guard<std::mutex> lock(windowsMutex);

            auto it = windows.find(handle);
            if (it == windows.end())
                return false;

            window = it->second;
            windows.erase(it);
            windowConfigs.erase(handle);
            windowIds.erase(handle);
            inputs.erase(handle);
            windowSizes.erase(handle);
        }

        renderHooks.erase(handle); // control-thread-only map, see its own comment -- safe unguarded here (destroyWindowInternal always runs on the control thread)

        SDL_DestroyWindow(window);
        return true;
    }

    WindowHandle WindowModule::Impl::handleForWindow(SDL_Window* window) const
    {
        std::lock_guard<std::mutex> lock(windowsMutex);

        for (const auto& [h, w] : windows)
            if (w == window)
                return h;

        return kInvalidWindowHandle;
    }

    WindowModule::Impl::WindowConfig WindowModule::Impl::configFor(WindowHandle handle) const
    {
        std::lock_guard<std::mutex> lock(windowsMutex);

        auto it = windowConfigs.find(handle);
        return it == windowConfigs.end() ? WindowConfig{} : it->second;
    }

    void WindowModule::Impl::handleSDLEvent(const SDL_Event& event)
    {
        switch (event.type)
        {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            {
                SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !context)
                    return;

                if (configFor(handle).autoHandleCloseEvent)
                {
                    destroyWindowInternal(handle);
                    context->comm().emit<WindowHandle>("window.closed", handle);
                }
                else
                {
                    context->comm().emit<WindowHandle>("window.close_requested", handle);
                }

                return;
            }

            case SDL_EVENT_WINDOW_RESIZED:
            {
                if (!context)
                    return;

                SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle)
                    return;

                {
                    std::lock_guard<std::mutex> lock(windowsMutex);
                    windowSizes[handle] = { event.window.data1, event.window.data2 };
                }

                WindowResizedEvent payload{ handle, event.window.data1, event.window.data2 };
                context->comm().emit<WindowResizedEvent>("window.resized", payload);
                return;
            }

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            {
                if (!context)
                    return;

                SDL_Window* window = SDL_GetWindowFromID(event.window.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle)
                    return;

                bool focused = (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
                WindowFocusEvent payload{ handle, focused };
                context->comm().emit<WindowFocusEvent>("window.focus_changed", payload);
                return;
            }

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            {
                SDL_Window* window = SDL_GetWindowFromID(event.key.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !configFor(handle).inputEnabled)
                    return;

                dsbox::Input* input = nullptr;
                {
                    std::lock_guard<std::mutex> lock(windowsMutex);
                    auto it = inputs.find(handle);
                    if (it != inputs.end())
                        input = it->second.get();
                }
                if (!input)
                    return;

                input->setKeyState(
                    translateScancode(event.key.scancode),
                    event.type == SDL_EVENT_KEY_DOWN,
                    event.key.repeat,
                    translateModifiers(static_cast<SDL_Keymod>(event.key.mod)));

                return;
            }

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            {
                SDL_Window* window = SDL_GetWindowFromID(event.button.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !configFor(handle).inputEnabled)
                    return;

                dsbox::Input* input = nullptr;
                {
                    std::lock_guard<std::mutex> lock(windowsMutex);
                    auto it = inputs.find(handle);
                    if (it != inputs.end())
                        input = it->second.get();
                }
                if (!input)
                    return;

                input->setMouseButtonState(
                    translateMouseButton(event.button.button),
                    event.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                    static_cast<int>(event.button.x),
                    static_cast<int>(event.button.y));

                // See heldMouseButtonCount's own comment: capture the
                // mouse for as long as any button is held, so a release
                // that happens after the cursor has left the window
                // still reaches us as a MOUSE_BUTTON_UP instead of being
                // silently dropped (which would otherwise leave this
                // button stuck "pressed" in dsbox::Input forever).
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                {
                    if (heldMouseButtonCount++ == 0)
                        SDL_CaptureMouse(true);
                }
                else if (heldMouseButtonCount > 0 && --heldMouseButtonCount == 0)
                {
                    SDL_CaptureMouse(false);
                }

                return;
            }

            case SDL_EVENT_MOUSE_MOTION:
            {
                SDL_Window* window = SDL_GetWindowFromID(event.motion.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !configFor(handle).inputEnabled)
                    return;

                dsbox::Input* input = nullptr;
                {
                    std::lock_guard<std::mutex> lock(windowsMutex);
                    auto it = inputs.find(handle);
                    if (it != inputs.end())
                        input = it->second.get();
                }
                if (!input)
                    return;

                input->setMousePosition(
                    static_cast<int>(event.motion.x),
                    static_cast<int>(event.motion.y),
                    static_cast<int>(event.motion.xrel),
                    static_cast<int>(event.motion.yrel));

                return;
            }

            case SDL_EVENT_MOUSE_WHEEL:
            {
                SDL_Window* window = SDL_GetWindowFromID(event.wheel.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !configFor(handle).inputEnabled)
                    return;

                dsbox::Input* input = nullptr;
                {
                    std::lock_guard<std::mutex> lock(windowsMutex);
                    auto it = inputs.find(handle);
                    if (it != inputs.end())
                        input = it->second.get();
                }
                if (!input)
                    return;

                input->setMouseWheel(event.wheel.x, event.wheel.y);
                return;
            }

            case SDL_EVENT_TEXT_INPUT:
            {
                if (!context)
                    return;

                SDL_Window* window = SDL_GetWindowFromID(event.text.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !configFor(handle).inputEnabled)
                    return;

                dsbox::TextInputEvent payload;
                payload.handle = handle;
                payload.text = event.text.text ? event.text.text : "";
                context->comm().emit<dsbox::TextInputEvent>("window.text_input", payload);
                return;
            }

            case SDL_EVENT_TEXT_EDITING:
            {
                if (!context)
                    return;

                SDL_Window* window = SDL_GetWindowFromID(event.edit.windowID);
                if (!window)
                    return;

                WindowHandle handle = handleForWindow(window);
                if (handle == kInvalidWindowHandle || !configFor(handle).inputEnabled)
                    return;

                // SDL3's `start` is a character offset into `text`, while
                // dsbox::TextEditingEvent documents UTF-8 BYTE offsets --
                // convert, so a consumer never has to know which unit
                // this particular backend happens to use.
                const std::string text = event.edit.text ? event.edit.text : "";

                auto byteOffsetOfChar = [&text](int charIndex) -> int
                {
                    int bytes = 0;
                    int chars = 0;
                    while (bytes < static_cast<int>(text.size()) && chars < charIndex)
                    {
                        ++bytes;
                        while (bytes < static_cast<int>(text.size()) &&
                               (static_cast<unsigned char>(text[bytes]) & 0xC0) == 0x80)
                            ++bytes; // skip UTF-8 continuation bytes
                        ++chars;
                    }
                    return bytes;
                };

                const int startChar = event.edit.start < 0 ? 0 : event.edit.start;
                const int endChar = startChar + (event.edit.length < 0 ? 0 : event.edit.length);
                const int cursorByte = byteOffsetOfChar(startChar);

                dsbox::TextEditingEvent payload;
                payload.handle = handle;
                payload.text = text;
                payload.cursor = cursorByte;
                payload.selectionLength = byteOffsetOfChar(endChar) - cursorByte;
                context->comm().emit<dsbox::TextEditingEvent>("window.text_editing", payload);
                return;
            }

            default:
                return; // out of scope for this module
        }
    }

    void WindowModule::Impl::runRenderHooks()
    {
        if (renderHooks.empty())
            return;

        // Snapshotted before calling out: purely defensive (renderHooks
        // is only ever touched on this same thread -- see its own
        // comment -- and a command posted from inside a hook's own
        // onFrame() would only run on a LATER iteration regardless, see
        // setRenderHook()'s own doc), so this protects against nothing
        // that can actually happen today, but keeps this loop correct
        // even if that ever changes.
        std::vector<std::pair<WindowHandle, dsbox::IWindowRenderHook*>> hooks(
            renderHooks.begin(), renderHooks.end());

        for (auto& [handle, hook] : hooks)
        {
            if (!hook)
                continue;

            SDL_Window* window = nullptr;
            {
                std::lock_guard<std::mutex> lock(windowsMutex);
                auto it = windows.find(handle);
                if (it != windows.end())
                    window = it->second;
            }

            if (!window)
                continue; // window closed since this hook was registered, but setRenderHook(handle, nullptr) hasn't caught up yet

            int width = 0, height = 0;
            SDL_GetWindowSizeInPixels(window, &width, &height);

            hook->onFrame(handle, width, height);
        }
    }

    void WindowModule::Impl::controlThreadLoop(std::promise<void> readyPromise)
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
        {
            setError(SDL_GetError());
            readyPromise.set_value();
            return; // control thread ends here; module stays inert
        }

        controlThreadId = std::this_thread::get_id();
        wakeEventType = SDL_RegisterEvents(1);
        initialized.store(true);

        readyPromise.set_value();

        SDL_Event event;
        while (running.load())
        {
            // renderHooks is only ever touched on this thread (see its
            // own comment), so reading .empty() here needs no lock.
            // With no hooks registered, this is exactly the original
            // unbounded SDL_WaitEvent() -- zero behavior change for an
            // application that never calls setRenderHook(). With at
            // least one hook, a short timeout lets the loop fall through
            // to runRenderHooks() below regularly even when nothing
            // native is happening; a real event (including our own
            // posted wake event) is still returned immediately the
            // moment it's queued, so this doesn't add latency to normal
            // event handling.
            bool gotEvent = renderHooks.empty()
                ? SDL_WaitEvent(&event)
                : SDL_WaitEventTimeout(&event, 1);

            if (gotEvent)
            {
                if (event.type == wakeEventType)
                    processCommands();
                else
                    handleSDLEvent(event);
            }

            runRenderHooks();
        }

        // Final drain, in case a command was posted right as shutdown
        // began (its wake event may already be queued behind this loop's
        // exit).
        processCommands();

        // Release the mouse capture (see heldMouseButtonCount's own
        // comment) if shutdown happens while a button is still held, so
        // SDL_Quit() below isn't asked to tear down a captured mouse.
        if (heldMouseButtonCount > 0)
        {
            heldMouseButtonCount = 0;
            SDL_CaptureMouse(false);
        }

        {
            std::lock_guard<std::mutex> lock(windowsMutex);
            for (auto& [handle, window] : windows)
                SDL_DestroyWindow(window);
            windows.clear();
            windowConfigs.clear();
            windowIds.clear();
            inputs.clear();
        }
        renderHooks.clear();

        SDL_Quit();
    }

    WindowModule::WindowModule() : WindowModule(WindowModuleOptions{})
    {
    }

    WindowModule::WindowModule(WindowModuleOptions options)
        : m_impl(std::make_unique<Impl>(std::move(options)))
    {
    }

    WindowModule::~WindowModule()
    {
        // Mirrors onShutdown(): safe to call even if onShutdown already
        // ran (m_impl->controlThread won't be joinable a second time),
        // and guards against abnormal teardown that skipped onShutdown.
        m_impl->running.store(false);
        m_impl->spaceAvailable.notify_all();

        if (m_impl->controlThread.joinable())
        {
            if (m_impl->initialized.load())
            {
                SDL_Event wakeEvent{};
                wakeEvent.type = m_impl->wakeEventType;
                SDL_PushEvent(&wakeEvent);
            }

            m_impl->controlThread.join();
        }
    }

    void WindowModule::onInit(dsbox::Context& context)
    {
        m_impl->context = &context;
        m_impl->running.store(true);

        std::promise<void> readyPromise;
        std::future<void> readyFuture = readyPromise.get_future();

        m_impl->controlThread = std::thread(
            &Impl::controlThreadLoop, m_impl.get(), std::move(readyPromise));

        // Blocks until the control thread has finished SDL_Init (success
        // or failure) and, on success, registered the wake event -- so
        // by the time onInit() returns, this module is fully usable (or
        // has recorded exactly why it isn't, via lastError()).
        readyFuture.wait();
    }

    void WindowModule::onShutdown(dsbox::Context&)
    {
        m_impl->running.store(false);
        m_impl->spaceAvailable.notify_all();

        if (m_impl->controlThread.joinable())
        {
            if (m_impl->initialized.load())
            {
                // Wakes the control thread if it's blocked in
                // SDL_WaitEvent, so it notices `running` is now false.
                SDL_Event wakeEvent{};
                wakeEvent.type = m_impl->wakeEventType;
                SDL_PushEvent(&wakeEvent);
            }

            m_impl->controlThread.join();
        }
    }

    dsbox::Input* WindowModule::inputFor(WindowHandle handle) const
    {
        std::lock_guard<std::mutex> lock(m_impl->windowsMutex);

        auto it = m_impl->inputs.find(handle);
        return it == m_impl->inputs.end() ? nullptr : it->second.get();
    }

    bool WindowModule::isKeyDown(WindowHandle handle, KeyCode key) const
    {
        dsbox::Input* input = inputFor(handle);
        return input && input->isKeyDown(key);
    }

    bool WindowModule::isMouseButtonDown(WindowHandle handle, MouseButton button) const
    {
        dsbox::Input* input = inputFor(handle);
        return input && input->isMouseButtonDown(button);
    }

    bool WindowModule::isWindowValid(WindowHandle handle) const
    {
        std::lock_guard<std::mutex> lock(m_impl->windowsMutex);
        return m_impl->windows.find(handle) != m_impl->windows.end();
    }

    bool WindowModule::mousePosition(WindowHandle handle, int& outX, int& outY) const
    {
        dsbox::Input* input = inputFor(handle);
        return input && input->mousePosition(outX, outY);
    }

    bool WindowModule::windowSize(WindowHandle handle, int& outWidth, int& outHeight) const
    {
        std::lock_guard<std::mutex> lock(m_impl->windowsMutex);

        auto it = m_impl->windowSizes.find(handle);
        if (it == m_impl->windowSizes.end())
            return false;

        outWidth = it->second.first;
        outHeight = it->second.second;
        return true;
    }

    std::uint32_t WindowModule::debugGetSDLWindowId(WindowHandle handle) const
    {
        std::lock_guard<std::mutex> lock(m_impl->windowsMutex);

        auto it = m_impl->windowIds.find(handle);
        return it == m_impl->windowIds.end() ? 0 : it->second;
    }

    std::string WindowModule::lastError() const
    {
        std::lock_guard<std::mutex> lock(m_impl->errorMutex);
        return m_impl->lastError;
    }

    WindowHandle WindowModule::createWindow(WindowOptions options)
    {
        if (!m_impl->initialized.load())
            return kInvalidWindowHandle;

        std::promise<WindowHandle> resultPromise;
        std::future<WindowHandle> resultFuture = resultPromise.get_future();

        Impl* impl = m_impl.get();

        impl->post(
            [impl, options = std::move(options), &resultPromise]() mutable
            {
                SDL_WindowFlags flags = options.resizable ? SDL_WINDOW_RESIZABLE : 0;

                switch (options.graphicsHint)
                {
                    case WindowGraphicsHint::Vulkan:
                        flags |= SDL_WINDOW_VULKAN;
                        break;
                    case WindowGraphicsHint::OpenGL:
                        flags |= SDL_WINDOW_OPENGL;
                        break;
                    case WindowGraphicsHint::None:
                        break;
                }

                SDL_Window* window = SDL_CreateWindow(
                    options.title.c_str(), options.width, options.height, flags);

                if (!window)
                {
                    impl->setError(SDL_GetError());
                    resultPromise.set_value(kInvalidWindowHandle);
                    return;
                }

                WindowHandle handle = impl->nextHandle.fetch_add(1);
                std::uint32_t windowId = SDL_GetWindowID(window);

                int initialWidth = 0, initialHeight = 0;
                SDL_GetWindowSizeInPixels(window, &initialWidth, &initialHeight);

                Impl::WindowConfig config;
                config.autoHandleCloseEvent = options.autoHandleCloseEvent;
                config.inputEnabled = options.inputEnabled;

                dsbox::InputOptions inputOptions;
                inputOptions.dispatchEvents = options.dispatchInputEvents;
                inputOptions.trackState = options.trackInputState;

                auto input = std::make_unique<dsbox::Input>(
                    impl->context->comm(), handle, inputOptions);

                {
                    std::lock_guard<std::mutex> lock(impl->windowsMutex);
                    impl->windows.emplace(handle, window);
                    impl->windowConfigs.emplace(handle, config);
                    impl->windowIds.emplace(handle, windowId);
                    impl->inputs.emplace(handle, std::move(input));
                    impl->windowSizes.emplace(handle, std::make_pair(initialWidth, initialHeight));
                }

                resultPromise.set_value(handle);
            });

        // Blocks the caller until the control thread has actually
        // created (or failed to create) the window.
        return resultFuture.get();
    }

    void WindowModule::closeWindow(WindowHandle handle)
    {
        if (!m_impl->initialized.load())
            return;

        std::promise<bool> donePromise;
        std::future<bool> doneFuture = donePromise.get_future();

        Impl* impl = m_impl.get();

        impl->post(
            [impl, handle, &donePromise]() mutable
            {
                donePromise.set_value(impl->destroyWindowInternal(handle));
            });

        if (doneFuture.get() && m_impl->context)
            m_impl->context->comm().emit<WindowHandle>("window.closed", handle);
    }

    void WindowModule::setWindowTitle(WindowHandle handle, std::string title)
    {
        if (!m_impl->initialized.load())
            return;

        Impl* impl = m_impl.get();

        impl->post(
            [impl, handle, title = std::move(title)]() mutable
            {
                std::lock_guard<std::mutex> lock(impl->windowsMutex);

                auto it = impl->windows.find(handle);
                if (it != impl->windows.end())
                    SDL_SetWindowTitle(it->second, title.c_str());
            });
    }

    void WindowModule::withNativeWindow(WindowHandle handle, std::function<void(SDL_Window*)> fn)
    {
        if (!m_impl->initialized.load() || !fn)
            return;

        std::promise<void> donePromise;
        std::future<void> doneFuture = donePromise.get_future();

        Impl* impl = m_impl.get();

        impl->post(
            [impl, handle, fn = std::move(fn), &donePromise]() mutable
            {
                SDL_Window* window = nullptr;

                {
                    std::lock_guard<std::mutex> lock(impl->windowsMutex);
                    auto it = impl->windows.find(handle);
                    if (it != impl->windows.end())
                        window = it->second;
                }

                if (window)
                    fn(window);

                donePromise.set_value();
            });

        // Blocks the caller until fn has actually run on the control
        // thread -- fn's whole point is to do something with a pointer
        // that's only valid on that thread, so "fire and forget" isn't
        // an option here the way it is for setWindowTitle().
        doneFuture.wait();
    }

    void WindowModule::withNativeWindow(dsbox::WindowHandle handle, std::function<void(void*)> fn)
    {
        if (!fn)
            return;

        // Delegates to the SDL_Window*-typed overload above rather than
        // duplicating its post()/promise/future plumbing -- this is
        // purely a type-erasure wrapper for dsbox::IWindowProvider
        // callers.
        withNativeWindow(handle, [fn = std::move(fn)](SDL_Window* window) { fn(window); });
    }

    void WindowModule::setRenderHook(WindowHandle handle, dsbox::IWindowRenderHook* hook)
    {
        if (!m_impl->initialized.load())
            return;

        std::promise<void> donePromise;
        std::future<void> doneFuture = donePromise.get_future();

        Impl* impl = m_impl.get();

        impl->post(
            [impl, handle, hook, &donePromise]() mutable
            {
                if (hook)
                    impl->renderHooks[handle] = hook;
                else
                    impl->renderHooks.erase(handle);

                donePromise.set_value();
            });

        // Blocks until the change has actually taken effect ON the
        // control thread -- see this method's own doc for why that
        // matters specifically when clearing a hook (hook == nullptr).
        doneFuture.wait();
    }

    // -- dsbox::ITextInputProvider --
    //
    // Each of the three methods below resolves its SDL_Window* under
    // windowsMutex, releases the lock, and only then makes the SDL call.
    // The lookup happens INSIDE the runOrPost() lambda, i.e. on the
    // control thread: a window closed between the call and a queued
    // command actually running is simply skipped rather than
    // dereferenced.
    void WindowModule::startTextInput(WindowHandle handle)
    {
        if (!m_impl->initialized.load())
            return;

        Impl* impl = m_impl.get();
        impl->runOrPost([impl, handle]
        {
            SDL_Window* window = nullptr;
            {
                std::lock_guard<std::mutex> lock(impl->windowsMutex);
                auto it = impl->windows.find(handle);
                if (it != impl->windows.end())
                    window = it->second;
            }

            if (window && !SDL_StartTextInput(window))
                impl->setError(SDL_GetError());
        });
    }

    void WindowModule::stopTextInput(WindowHandle handle)
    {
        if (!m_impl->initialized.load())
            return;

        Impl* impl = m_impl.get();
        impl->runOrPost([impl, handle]
        {
            SDL_Window* window = nullptr;
            {
                std::lock_guard<std::mutex> lock(impl->windowsMutex);
                auto it = impl->windows.find(handle);
                if (it != impl->windows.end())
                    window = it->second;
            }

            if (window && !SDL_StopTextInput(window))
                impl->setError(SDL_GetError());
        });
    }

    void WindowModule::setTextInputRect(WindowHandle handle, int x, int y, int width, int height)
    {
        if (!m_impl->initialized.load())
            return;

        Impl* impl = m_impl.get();
        impl->runOrPost([impl, handle, x, y, width, height]
        {
            SDL_Window* window = nullptr;
            {
                std::lock_guard<std::mutex> lock(impl->windowsMutex);
                auto it = impl->windows.find(handle);
                if (it != impl->windows.end())
                    window = it->second;
            }

            if (!window)
                return;

            const SDL_Rect rect{ x, y, width, height };
            if (!SDL_SetTextInputArea(window, &rect, 0))
                impl->setError(SDL_GetError());
        });
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_sdl::WindowModule)
