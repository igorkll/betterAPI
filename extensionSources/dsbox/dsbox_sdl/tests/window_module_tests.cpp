#include "test_framework.hpp"

#include <dsbox_sdl/window_module.hpp>

#include <dsbox/context.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    void testCreateAndCloseWindow()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        bool closedFired = false;
        dsbox_sdl::WindowHandle closedHandle = dsbox_sdl::kInvalidWindowHandle;

        context.comm().registerEvent<dsbox_sdl::WindowHandle>(
            "window.closed",
            [&](dsbox_sdl::WindowHandle h)
            {
                std::lock_guard<std::mutex> lock(mutex);
                closedHandle = h;
                closedFired = true;
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options;
        options.title = "Test Window";

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        DSBOX_SDL_CHECK(handle != dsbox_sdl::kInvalidWindowHandle);

        window.closeWindow(handle);

        std::unique_lock<std::mutex> lock(mutex);
        bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return closedFired; });
        DSBOX_SDL_CHECK(ok);
        DSBOX_SDL_CHECK_EQ(closedHandle, handle);
        lock.unlock();

        window.onShutdown(context);
    }

    void testCreateWindowBeforeInitReturnsInvalid()
    {
        dsbox_sdl::WindowModule window;
        // Deliberately not calling onInit() -- must return immediately,
        // not hang waiting on a control thread that doesn't exist.
        dsbox_sdl::WindowHandle handle = window.createWindow(dsbox_sdl::WindowOptions{});
        DSBOX_SDL_CHECK_EQ(handle, dsbox_sdl::kInvalidWindowHandle);
    }

    void testCloseUnknownHandleIsNoop()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::atomic<int> closedCount{ 0 };
        context.comm().registerEvent<dsbox_sdl::WindowHandle>(
            "window.closed", [&](dsbox_sdl::WindowHandle) { closedCount++; });

        window.closeWindow(999999); // never created

        DSBOX_SDL_CHECK_EQ(closedCount.load(), 0);

        window.onShutdown(context);
    }

    // A window with autoHandleCloseEvent = true (the default): the
    // control thread must destroy the window itself and publish
    // "window.closed" the moment a close is requested.
    void testAutoHandleCloseEventDestroysWindow()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        bool closedFired = false;
        dsbox_sdl::WindowHandle closedHandle = dsbox_sdl::kInvalidWindowHandle;

        context.comm().registerEvent<dsbox_sdl::WindowHandle>(
            "window.closed",
            [&](dsbox_sdl::WindowHandle h)
            {
                std::lock_guard<std::mutex> lock(mutex);
                closedHandle = h;
                closedFired = true;
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options;
        options.autoHandleCloseEvent = true;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        DSBOX_SDL_CHECK(handle != dsbox_sdl::kInvalidWindowHandle);

        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);
        DSBOX_SDL_CHECK(sdlId != 0);

        // Simulates the OS/window-manager sending a close request (e.g.
        // the user clicked the titlebar X).
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event.window.windowID = sdlId;
        SDL_PushEvent(&event);

        std::unique_lock<std::mutex> lock(mutex);
        bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return closedFired; });
        DSBOX_SDL_CHECK(ok);
        DSBOX_SDL_CHECK_EQ(closedHandle, handle);
        lock.unlock();

        // Already destroyed by the auto-handle path -- a follow-up
        // explicit close must be a harmless no-op, not a crash/hang.
        window.closeWindow(handle);

        window.onShutdown(context);
    }

    // A window with autoHandleCloseEvent = false: a close request must
    // publish "window.close_requested" WITHOUT destroying anything,
    // leaving the decision to the application.
    void testManualHandleCloseEventPublishesRequestOnly()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        bool requestFired = false;
        bool closedFired = false;
        dsbox_sdl::WindowHandle requestedHandle = dsbox_sdl::kInvalidWindowHandle;

        context.comm().registerEvent<dsbox_sdl::WindowHandle>(
            "window.close_requested",
            [&](dsbox_sdl::WindowHandle h)
            {
                std::lock_guard<std::mutex> lock(mutex);
                requestedHandle = h;
                requestFired = true;
                cv.notify_one();
            });

        context.comm().registerEvent<dsbox_sdl::WindowHandle>(
            "window.closed",
            [&](dsbox_sdl::WindowHandle)
            {
                std::lock_guard<std::mutex> lock(mutex);
                closedFired = true;
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options;
        options.autoHandleCloseEvent = false;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        DSBOX_SDL_CHECK(handle != dsbox_sdl::kInvalidWindowHandle);

        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);

        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event.window.windowID = sdlId;
        SDL_PushEvent(&event);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return requestFired; });
            DSBOX_SDL_CHECK(ok);
            DSBOX_SDL_CHECK_EQ(requestedHandle, handle);
            DSBOX_SDL_CHECK(!closedFired); // must NOT have been auto-destroyed
        }

        // The window must still be alive: still resolvable to an SDL ID
        // (debugGetSDLWindowId only forgets a handle once actually
        // destroyed).
        DSBOX_SDL_CHECK(window.debugGetSDLWindowId(handle) != 0);

        // The application now explicitly closes it.
        window.closeWindow(handle);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return closedFired; });
            DSBOX_SDL_CHECK(ok);
        }

        window.onShutdown(context);
    }

    void testConcurrentWindowCreationFromMultipleThreads()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        constexpr int kThreads = 8;

        std::mutex handlesMutex;
        std::vector<dsbox_sdl::WindowHandle> handles;
        std::vector<std::thread> creators;

        for (int t = 0; t < kThreads; ++t)
        {
            creators.emplace_back([&]
            {
                dsbox_sdl::WindowOptions options;
                options.title = "Concurrent";

                dsbox_sdl::WindowHandle handle = window.createWindow(options);

                std::lock_guard<std::mutex> lock(handlesMutex);
                handles.push_back(handle);
            });
        }

        for (auto& t : creators)
            t.join();

        DSBOX_SDL_CHECK_EQ(handles.size(), static_cast<std::size_t>(kThreads));

        std::vector<dsbox_sdl::WindowHandle> sorted = handles;
        std::sort(sorted.begin(), sorted.end());

        bool allValid = true;
        bool allDistinct = true;

        for (std::size_t i = 0; i < sorted.size(); ++i)
        {
            if (sorted[i] == dsbox_sdl::kInvalidWindowHandle)
                allValid = false;

            if (i > 0 && sorted[i] == sorted[i - 1])
                allDistinct = false;
        }

        DSBOX_SDL_CHECK(allValid);
        DSBOX_SDL_CHECK(allDistinct);

        window.onShutdown(context);
    }

    void testShutdownDestroysRemainingWindowsWithoutHanging()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        dsbox_sdl::WindowHandle handle = window.createWindow(dsbox_sdl::WindowOptions{});
        DSBOX_SDL_CHECK(handle != dsbox_sdl::kInvalidWindowHandle);

        // Deliberately not closing it -- onShutdown() must clean it up
        // itself without hanging.
        window.onShutdown(context);
    }

    void testSetWindowTitleDoesNotCrash()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        dsbox_sdl::WindowHandle handle = window.createWindow(dsbox_sdl::WindowOptions{});
        window.setWindowTitle(handle, "Renamed");
        window.setWindowTitle(999999, "Unknown handle -- must be a no-op");

        window.onShutdown(context);
    }

    void testShutdownIsIdempotent()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);
        window.onShutdown(context);
        window.onShutdown(context); // must not crash, hang, or double-join
    }

    void testDestructorStopsControlThreadSafely()
    {
        dsbox::Context context;
        {
            dsbox_sdl::WindowModule window;
            window.onInit(context);
            window.createWindow(dsbox_sdl::WindowOptions{});
            // No explicit onShutdown() -- the destructor must stop and
            // join the control thread (destroying the window) safely on
            // its own.
        }
    }

    // withNativeWindow() is the generic renderer-attachment hook: verify
    // it hands back a pointer to the SAME SDL_Window the handle refers
    // to (checked indirectly via SDL_GetWindowID, since application code
    // has no other legitimate way to obtain an SDL window ID) and that
    // it's a safe no-op for a handle that doesn't exist.
    void testWithNativeWindowProvidesCorrectWindow()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        dsbox_sdl::WindowHandle handle = window.createWindow(dsbox_sdl::WindowOptions{});
        std::uint32_t expectedId = window.debugGetSDLWindowId(handle);
        DSBOX_SDL_CHECK(expectedId != 0);

        std::uint32_t observedId = 0;
        bool called = false;

        window.withNativeWindow(
            handle,
            [&](SDL_Window* nativeWindow)
            {
                called = true;
                observedId = SDL_GetWindowID(nativeWindow);
            });

        DSBOX_SDL_CHECK(called);
        DSBOX_SDL_CHECK_EQ(observedId, expectedId);

        // Unknown handle: fn must never be invoked, and the call must
        // still return promptly rather than hang.
        bool calledForUnknown = false;
        window.withNativeWindow(
            static_cast<dsbox_sdl::WindowHandle>(999999),
            [&](SDL_Window*) { calledForUnknown = true; });

        DSBOX_SDL_CHECK(!calledForUnknown);

        window.onShutdown(context);
    }

    // Dispatch + tracking both enabled (the default): a synthetic key
    // press/release must both fire window.key AND update isKeyDown().
    void testKeyEventDispatchesAndTracksState()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<dsbox_sdl::KeyEvent> received;

        context.comm().registerEvent<dsbox_sdl::KeyEvent>(
            "window.key",
            [&](dsbox_sdl::KeyEvent e)
            {
                std::lock_guard<std::mutex> lock(mutex);
                received.push_back(e);
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options;
        options.trackInputState = true;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        DSBOX_SDL_CHECK(handle != dsbox_sdl::kInvalidWindowHandle);

        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);
        DSBOX_SDL_CHECK(sdlId != 0);

        DSBOX_SDL_CHECK(!window.isKeyDown(handle, dsbox_sdl::KeyCode::A));

        SDL_Event down{};
        down.type = SDL_EVENT_KEY_DOWN;
        down.key.windowID = sdlId;
        down.key.scancode = SDL_SCANCODE_A;
        down.key.mod = SDL_KMOD_NONE;
        down.key.repeat = false;
        SDL_PushEvent(&down);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return !received.empty(); });
            DSBOX_SDL_CHECK(ok);
        }

        DSBOX_SDL_CHECK(window.isKeyDown(handle, dsbox_sdl::KeyCode::A));

        SDL_Event up{};
        up.type = SDL_EVENT_KEY_UP;
        up.key.windowID = sdlId;
        up.key.scancode = SDL_SCANCODE_A;
        up.key.mod = SDL_KMOD_NONE;
        up.key.repeat = false;
        SDL_PushEvent(&up);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return received.size() >= 2; });
            DSBOX_SDL_CHECK(ok);
        }

        DSBOX_SDL_CHECK(!window.isKeyDown(handle, dsbox_sdl::KeyCode::A));

        DSBOX_SDL_CHECK_EQ(received.size(), static_cast<std::size_t>(2));
        if (received.size() >= 2)
        {
            DSBOX_SDL_CHECK(received[0].key == dsbox_sdl::KeyCode::A);
            DSBOX_SDL_CHECK(received[0].pressed);
            DSBOX_SDL_CHECK(!received[1].pressed);
        }

        window.onShutdown(context);
    }

    // inputEnabled = false must suppress both dispatch and tracking
    // entirely for that window.
    void testInputDisabledSuppressesEverything()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::atomic<int> keyEvents{ 0 };
        context.comm().registerEvent<dsbox_sdl::KeyEvent>(
            "window.key", [&](dsbox_sdl::KeyEvent) { keyEvents++; });

        dsbox_sdl::WindowOptions options;
        options.inputEnabled = false;
        options.trackInputState = true; // must be ignored -- inputEnabled wins

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);

        SDL_Event down{};
        down.type = SDL_EVENT_KEY_DOWN;
        down.key.windowID = sdlId;
        down.key.scancode = SDL_SCANCODE_A;
        SDL_PushEvent(&down);

        // No reliable event to wait on here (nothing should fire) -- a
        // short, bounded sleep is the pragmatic way to assert an absence.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        DSBOX_SDL_CHECK_EQ(keyEvents.load(), 0);
        DSBOX_SDL_CHECK(!window.isKeyDown(handle, dsbox_sdl::KeyCode::A));

        window.onShutdown(context);
    }

    // dispatchInputEvents = false, trackInputState = true: state must
    // still update even though nothing is published on the bus.
    void testTrackingWithoutDispatch()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::atomic<int> keyEvents{ 0 };
        context.comm().registerEvent<dsbox_sdl::KeyEvent>(
            "window.key", [&](dsbox_sdl::KeyEvent) { keyEvents++; });

        dsbox_sdl::WindowOptions options;
        options.dispatchInputEvents = false;
        options.trackInputState = true;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);

        SDL_Event down{};
        down.type = SDL_EVENT_KEY_DOWN;
        down.key.windowID = sdlId;
        down.key.scancode = SDL_SCANCODE_SPACE;
        SDL_PushEvent(&down);

        // Poll isKeyDown() with a timeout instead of a fixed sleep, since
        // there's no event to wait on: state updates asynchronously on
        // the control thread.
        bool observed = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (window.isKeyDown(handle, dsbox_sdl::KeyCode::Space))
            {
                observed = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        DSBOX_SDL_CHECK(observed);
        DSBOX_SDL_CHECK_EQ(keyEvents.load(), 0);

        window.onShutdown(context);
    }

    // dispatchInputEvents = true, trackInputState = false (the default
    // for tracking): events fire but isKeyDown()/mousePosition() report
    // nothing, since no InputState was ever created for this window.
    void testDispatchWithoutTracking()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        bool fired = false;

        context.comm().registerEvent<dsbox_sdl::KeyEvent>(
            "window.key",
            [&](dsbox_sdl::KeyEvent)
            {
                std::lock_guard<std::mutex> lock(mutex);
                fired = true;
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options; // trackInputState defaults to false
        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);

        SDL_Event down{};
        down.type = SDL_EVENT_KEY_DOWN;
        down.key.windowID = sdlId;
        down.key.scancode = SDL_SCANCODE_A;
        SDL_PushEvent(&down);

        std::unique_lock<std::mutex> lock(mutex);
        bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return fired; });
        DSBOX_SDL_CHECK(ok);
        lock.unlock();

        int outX = 0, outY = 0;
        DSBOX_SDL_CHECK(!window.isKeyDown(handle, dsbox_sdl::KeyCode::A));
        DSBOX_SDL_CHECK(!window.mousePosition(handle, outX, outY));

        window.onShutdown(context);
    }

    // Mouse button, motion, and wheel: one combined test covering all
    // three, since they share the same setup and the tracked-state
    // assertions build on each other naturally (position from motion,
    // buttons from button events).
    void testMouseButtonMotionAndWheelEvents()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        int buttonEvents = 0;
        int motionEvents = 0;
        int wheelEvents = 0;

        context.comm().registerEvent<dsbox_sdl::MouseButtonEvent>(
            "window.mouse_button",
            [&](dsbox_sdl::MouseButtonEvent)
            {
                std::lock_guard<std::mutex> lock(mutex);
                buttonEvents++;
                cv.notify_one();
            });

        context.comm().registerEvent<dsbox_sdl::MouseMotionEvent>(
            "window.mouse_motion",
            [&](dsbox_sdl::MouseMotionEvent)
            {
                std::lock_guard<std::mutex> lock(mutex);
                motionEvents++;
                cv.notify_one();
            });

        context.comm().registerEvent<dsbox_sdl::MouseWheelEvent>(
            "window.mouse_wheel",
            [&](dsbox_sdl::MouseWheelEvent)
            {
                std::lock_guard<std::mutex> lock(mutex);
                wheelEvents++;
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options;
        options.trackInputState = true;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);

        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = sdlId;
        motion.motion.x = 42.0f;
        motion.motion.y = 24.0f;
        motion.motion.xrel = 5.0f;
        motion.motion.yrel = -3.0f;
        SDL_PushEvent(&motion);

        SDL_Event buttonDown{};
        buttonDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        buttonDown.button.windowID = sdlId;
        buttonDown.button.button = SDL_BUTTON_LEFT;
        buttonDown.button.x = 42.0f;
        buttonDown.button.y = 24.0f;
        SDL_PushEvent(&buttonDown);

        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.windowID = sdlId;
        wheel.wheel.x = 0.0f;
        wheel.wheel.y = 1.0f;
        SDL_PushEvent(&wheel);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(
                lock,
                std::chrono::seconds(5),
                [&] { return motionEvents >= 1 && buttonEvents >= 1 && wheelEvents >= 1; });
            DSBOX_SDL_CHECK(ok);
        }

        DSBOX_SDL_CHECK(window.isMouseButtonDown(handle, dsbox_sdl::MouseButton::Left));

        int outX = 0, outY = 0;
        DSBOX_SDL_CHECK(window.mousePosition(handle, outX, outY));
        DSBOX_SDL_CHECK_EQ(outX, 42);
        DSBOX_SDL_CHECK_EQ(outY, 24);

        window.onShutdown(context);
    }

    // Regression test for "releasing the mouse button off-screen leaves
    // it stuck pressed": a MOUSE_BUTTON_UP with coordinates well outside
    // the window's bounds (simulating a release after the cursor left
    // the window) must still clear isMouseButtonDown(). SDL_PushEvent()
    // here injects the event directly rather than going through SDL's
    // real OS delivery path, so this covers the coordinates-out-of-bounds
    // half of the scenario; the delivery half (SDL not sending the event
    // at all without a mouse grab) is covered by the SDL_CaptureMouse()
    // fix in window_module.cpp (see heldMouseButtonCount).
    void testMouseButtonReleasedOffScreenClearsState()
    {
        dsbox::Context context;
        dsbox_sdl::WindowModule window;
        window.onInit(context);

        std::mutex mutex;
        std::condition_variable cv;
        int buttonEvents = 0;

        context.comm().registerEvent<dsbox_sdl::MouseButtonEvent>(
            "window.mouse_button",
            [&](dsbox_sdl::MouseButtonEvent)
            {
                std::lock_guard<std::mutex> lock(mutex);
                buttonEvents++;
                cv.notify_one();
            });

        dsbox_sdl::WindowOptions options;
        options.trackInputState = true;
        options.width = 200;
        options.height = 200;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);
        std::uint32_t sdlId = window.debugGetSDLWindowId(handle);

        SDL_Event buttonDown{};
        buttonDown.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        buttonDown.button.windowID = sdlId;
        buttonDown.button.button = SDL_BUTTON_LEFT;
        buttonDown.button.x = 50.0f;
        buttonDown.button.y = 50.0f;
        SDL_PushEvent(&buttonDown);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return buttonEvents >= 1; });
            DSBOX_SDL_CHECK(ok);
        }

        DSBOX_SDL_CHECK(window.isMouseButtonDown(handle, dsbox_sdl::MouseButton::Left));

        // Coordinates far outside the 200x200 window -- as if the
        // release happened well off-screen, which a real captured mouse
        // still reports to this window.
        SDL_Event buttonUp{};
        buttonUp.type = SDL_EVENT_MOUSE_BUTTON_UP;
        buttonUp.button.windowID = sdlId;
        buttonUp.button.button = SDL_BUTTON_LEFT;
        buttonUp.button.x = -500.0f;
        buttonUp.button.y = -500.0f;
        SDL_PushEvent(&buttonUp);

        {
            std::unique_lock<std::mutex> lock(mutex);
            bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return buttonEvents >= 2; });
            DSBOX_SDL_CHECK(ok);
        }

        DSBOX_SDL_CHECK(!window.isMouseButtonDown(handle, dsbox_sdl::MouseButton::Left));

        window.onShutdown(context);
    }
}

void runWindowModuleTests()
{
    using namespace dsbox_sdl_tests;

    runTest("CreateAndCloseWindow", testCreateAndCloseWindow);
    runTest("CreateWindowBeforeInitReturnsInvalid", testCreateWindowBeforeInitReturnsInvalid);
    runTest("CloseUnknownHandleIsNoop", testCloseUnknownHandleIsNoop);
    runTest("AutoHandleCloseEventDestroysWindow", testAutoHandleCloseEventDestroysWindow);
    runTest(
        "ManualHandleCloseEventPublishesRequestOnly",
        testManualHandleCloseEventPublishesRequestOnly);
    runTest(
        "ConcurrentWindowCreationFromMultipleThreads",
        testConcurrentWindowCreationFromMultipleThreads);
    runTest(
        "ShutdownDestroysRemainingWindowsWithoutHanging",
        testShutdownDestroysRemainingWindowsWithoutHanging);
    runTest("SetWindowTitleDoesNotCrash", testSetWindowTitleDoesNotCrash);
    runTest("ShutdownIsIdempotent", testShutdownIsIdempotent);
    runTest("DestructorStopsControlThreadSafely", testDestructorStopsControlThreadSafely);
    runTest("WithNativeWindowProvidesCorrectWindow", testWithNativeWindowProvidesCorrectWindow);

    runTest("KeyEventDispatchesAndTracksState", testKeyEventDispatchesAndTracksState);
    runTest("InputDisabledSuppressesEverything", testInputDisabledSuppressesEverything);
    runTest("TrackingWithoutDispatch", testTrackingWithoutDispatch);
    runTest("DispatchWithoutTracking", testDispatchWithoutTracking);
    runTest("MouseButtonMotionAndWheelEvents", testMouseButtonMotionAndWheelEvents);
    runTest(
        "MouseButtonReleasedOffScreenClearsState",
        testMouseButtonReleasedOffScreenClearsState);
}
