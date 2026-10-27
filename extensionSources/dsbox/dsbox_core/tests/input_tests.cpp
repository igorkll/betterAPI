#include "test_framework.hpp"

#include <dsbox/input.hpp>

#include <vector>

namespace
{
    // Dispatch + tracking both enabled: a key press/release must both
    // fire "window.key" AND update isKeyDown().
    void testKeyStateDispatchesAndTracks()
    {
        dsbox::CommunicationManager bus;

        std::vector<dsbox::KeyEvent> received;
        bus.registerEvent<dsbox::KeyEvent>(
            "window.key", [&](dsbox::KeyEvent e) { received.push_back(e); });

        dsbox::InputOptions options;
        options.trackState = true;

        dsbox::Input input(bus, 42, options);

        DSBOX_CHECK(!input.isKeyDown(dsbox::KeyCode::A));

        input.setKeyState(dsbox::KeyCode::A, true);
        DSBOX_CHECK(input.isKeyDown(dsbox::KeyCode::A));

        input.setKeyState(dsbox::KeyCode::A, false);
        DSBOX_CHECK(!input.isKeyDown(dsbox::KeyCode::A));

        DSBOX_CHECK_EQ(received.size(), static_cast<std::size_t>(2));
        if (received.size() == 2)
        {
            DSBOX_CHECK_EQ(received[0].handle, static_cast<dsbox::WindowHandle>(42));
            DSBOX_CHECK(received[0].key == dsbox::KeyCode::A);
            DSBOX_CHECK(received[0].pressed);
            DSBOX_CHECK(!received[1].pressed);
        }
    }

    // dispatchEvents = false, trackState = true: state updates even
    // though nothing is published.
    void testTrackingWithoutDispatch()
    {
        dsbox::CommunicationManager bus;

        int keyEvents = 0;
        bus.registerEvent<dsbox::KeyEvent>("window.key", [&](dsbox::KeyEvent) { keyEvents++; });

        dsbox::InputOptions options;
        options.dispatchEvents = false;
        options.trackState = true;

        dsbox::Input input(bus, 1, options);
        input.setKeyState(dsbox::KeyCode::Space, true);

        DSBOX_CHECK(input.isKeyDown(dsbox::KeyCode::Space));
        DSBOX_CHECK_EQ(keyEvents, 0);
    }

    // dispatchEvents = true, trackState = false (the default): events
    // fire but isKeyDown()/mousePosition() report nothing.
    void testDispatchWithoutTracking()
    {
        dsbox::CommunicationManager bus;

        int keyEvents = 0;
        bus.registerEvent<dsbox::KeyEvent>("window.key", [&](dsbox::KeyEvent) { keyEvents++; });

        dsbox::Input input(bus, 1); // default options: dispatch on, tracking off
        input.setKeyState(dsbox::KeyCode::A, true);

        DSBOX_CHECK_EQ(keyEvents, 1);
        DSBOX_CHECK(!input.isKeyDown(dsbox::KeyCode::A));

        int outX = 0, outY = 0;
        DSBOX_CHECK(!input.mousePosition(outX, outY));
    }

    // Mouse button + motion + wheel: button/motion update tracked
    // state, wheel never does (see MouseWheelEvent's own comment).
    void testMouseButtonMotionAndWheel()
    {
        dsbox::CommunicationManager bus;

        int buttonEvents = 0;
        int motionEvents = 0;
        int wheelEvents = 0;

        bus.registerEvent<dsbox::MouseButtonEvent>(
            "window.mouse_button", [&](dsbox::MouseButtonEvent) { buttonEvents++; });
        bus.registerEvent<dsbox::MouseMotionEvent>(
            "window.mouse_motion", [&](dsbox::MouseMotionEvent) { motionEvents++; });
        bus.registerEvent<dsbox::MouseWheelEvent>(
            "window.mouse_wheel", [&](dsbox::MouseWheelEvent) { wheelEvents++; });

        dsbox::InputOptions options;
        options.trackState = true;

        dsbox::Input input(bus, 7, options);

        input.setMousePosition(42, 24, 5, -3);
        input.setMouseButtonState(dsbox::MouseButton::Left, true, 42, 24);
        input.setMouseWheel(0.0f, 1.0f);

        DSBOX_CHECK_EQ(motionEvents, 1);
        DSBOX_CHECK_EQ(buttonEvents, 1);
        DSBOX_CHECK_EQ(wheelEvents, 1);

        DSBOX_CHECK(input.isMouseButtonDown(dsbox::MouseButton::Left));

        int outX = 0, outY = 0;
        DSBOX_CHECK(input.mousePosition(outX, outY));
        DSBOX_CHECK_EQ(outX, 42);
        DSBOX_CHECK_EQ(outY, 24);
    }

    // Two Input instances for two different handles must not see each
    // other's state, and events from one must not fire the other's
    // subscribers' expectations about which handle they belong to.
    void testInstancesAreIndependentPerHandle()
    {
        dsbox::CommunicationManager bus;

        std::vector<dsbox::WindowHandle> handlesSeen;
        bus.registerEvent<dsbox::KeyEvent>(
            "window.key", [&](dsbox::KeyEvent e) { handlesSeen.push_back(e.handle); });

        dsbox::InputOptions tracked;
        tracked.trackState = true;

        dsbox::Input first(bus, 1, tracked);
        dsbox::Input second(bus, 2, tracked);

        first.setKeyState(dsbox::KeyCode::A, true);

        DSBOX_CHECK(first.isKeyDown(dsbox::KeyCode::A));
        DSBOX_CHECK(!second.isKeyDown(dsbox::KeyCode::A));

        DSBOX_CHECK_EQ(handlesSeen.size(), static_cast<std::size_t>(1));
        if (!handlesSeen.empty())
            DSBOX_CHECK_EQ(handlesSeen[0], static_cast<dsbox::WindowHandle>(1));
    }
}

void runInputTests()
{
    using namespace dsbox_tests;

    runTest("KeyStateDispatchesAndTracks", testKeyStateDispatchesAndTracks);
    runTest("TrackingWithoutDispatch", testTrackingWithoutDispatch);
    runTest("DispatchWithoutTracking", testDispatchWithoutTracking);
    runTest("MouseButtonMotionAndWheel", testMouseButtonMotionAndWheel);
    runTest("InstancesAreIndependentPerHandle", testInstancesAreIndependentPerHandle);
}
