# dsbox_sdl — Agent Notes

## Purpose
SDL3-backed implementation of windowing + raw input for the rest of the
framework. Implements `dsbox::IWindowProvider` and
`dsbox::ITextInputProvider` (both owned by `dsbox_core`) so modules that
need "a native window" or "text input" never depend on SDL directly —
this module is the one and only place SDL headers are included.

## Key type
`WindowModule` (`window_module.hpp/cpp`) — owns one dedicated "control
thread" that runs SDL's own event loop (`SDL_WaitEvent`/
`SDL_WaitEventTimeout`); every SDL call happens there. Every public method
either runs inline when already on that thread (`runOrPost`, used by the
`ITextInputProvider` calls) or posts a command and blocks on a
`std::promise`/`std::future` until the control thread actually ran it
(`createWindow`, `closeWindow`, `withNativeWindow`, `setRenderHook`).
Raw SDL events are translated in `handleSDLEvent()` into
`dsbox::Input`/bus events (`"window.key"`, `"window.mouse_button"`, etc.)
that the rest of the framework (notably `dsbox_ui`) consumes without ever
touching an `SDL_Event` itself.

## Bug fixed this pass
**Releasing the mouse button after the cursor has left the window left
that button permanently "pressed" in `dsbox::Input`.** Root cause: SDL
does not deliver `SDL_EVENT_MOUSE_BUTTON_UP` (or motion) to a window once
the cursor has left its bounds, unless the mouse is explicitly captured
(`SDL_CaptureMouse`). So a press-drag-release-outside-the-window gesture
simply never produced a button-up event, and `Input::m_pressedButtons`
(see `dsbox_core`) never cleared that button.

Fix: `WindowModule::Impl` now tracks `heldMouseButtonCount` (control-
thread-only, no separate mutex needed — see its own comment) and calls
`SDL_CaptureMouse(true)` when the first button goes down, `SDL_CaptureMouse
(false)` once the last one comes back up (also released defensively on
shutdown with a button still held, before `SDL_Quit()`). This guarantees
the up event is still delivered to this window even with out-of-bounds
coordinates.

This is the OS/input-delivery half of the bug; see `dsbox_ui`'s own
`Agent.md` for the UI-layer half (pointer capture in `WindowUiTree`),
which is what actually keeps a `Container`'s pressed-color state from
getting stuck once the raw event *is* delivered.

Regression test: `testMouseButtonReleasedOffScreenClearsState` in
`tests/window_module_tests.cpp` — it can only exercise the
coordinates-out-of-bounds half directly (`SDL_PushEvent` injects the
event, bypassing real OS delivery), so it documents rather than fully
replicates the capture fix; read together with the comment on
`heldMouseButtonCount` in `window_module.cpp`.

## Known-subtle areas
- Every mutation of `renderHooks` (the render-hook map) happens only
  on the control thread by construction (`setRenderHook()` posts and
  blocks) — do not add a path that touches it from another thread
  without also adding a mutex.
- `startTextInput`/`stopTextInput`/`setTextInputRect` use `runOrPost()`,
  not `post()`, specifically because they're typically called from
  inside a `dsbox_ui` event handler that is *already* running on the
  control thread — see that method's own comment for why queuing there
  would be wrong, not just slower.

## Testing
`tests/window_module_tests.cpp`, shared `test_framework.hpp` runner.
Needs a real (possibly headless/virtual) display for SDL to create
windows against.
