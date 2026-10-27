# dsbox_core — Agent Notes

## Purpose
Foundation module: module lifecycle, the inter-module message bus, dynamic
plugin loading, and the small environment-abstracting interfaces that let
other modules avoid depending on a concrete windowing/GUI backend.

## Key types
- `IModule` / `ModuleManager` (`module.hpp`, `module_manager.hpp`) — every
  module implements `IModule` (name, dependencies, onInit/onStart/onStop/
  onShutdown). `ModuleManager` resolves a dependency order (Kahn's
  algorithm), runs the lifecycle in two full passes (all onInit, then all
  onStart; mirrored in reverse for stop/shutdown), and optionally isolates
  a failing module (+ its transitive dependents) via `ModuleErrorHandler`
  instead of aborting everything. All public methods are thread-safe; the
  internal mutex is never held while an `IModule` callback runs, so a
  module may safely call back into the manager from its own callback.
- `CommunicationManager` (`communication_manager.hpp`) — typed, name-keyed
  request/response handlers (at most one per name+signature), pub/sub
  events (copy-on-write subscriber lists so `emit()` never races
  register/unregister), and "handled events" (`registerHandledEvent` /
  `publishHandledEvent`): pub/sub like events, but every subscriber
  returns a value and `publishHandledEvent()` hands the publisher back
  one `HandledEventResult<RT>` (the value, or the `exception_ptr` if that
  subscriber threw) per subscriber, in subscription order. Fills the gap
  between a handler (exactly one answer, duplicate registration is an
  error) and a plain event (any number of listeners, nothing comes back).
  `HandlerHandle`/`EventHandle`/`HandledEventHandle` are weak references
  that go invalid instead of dangling when unregistered.
- `Overridable<Signature>` — "fallback with override hook" pattern built
  on `CommunicationManager`.
- `ModuleLoader` / `DynamicLibrary` — cross-platform (Win32/dlopen) .dll/.so
  loading, with an optional plugin-declared core-version check
  (`VersionRequirement`) performed before the plugin's factory is called.
- `IWindowProvider`, `ITextInputProvider` — core-owned interfaces so a
  module needing "the native window/text-input behind this handle" never
  depends on a concrete backend module (e.g. `dsbox_sdl`). **This is the
  main environment-agnostic seam in the whole framework — when adding a
  new module that needs something environment-specific, prefer adding an
  interface here over a hard dependency on a backend module.**

## Known-correct-but-subtle areas (read before touching)
- `ModuleManager::stopAndShutdown` deliberately runs two full passes
  (all onStop, then all onShutdown) for the same reason `initAndStart`
  does — don't collapse them into one pass per module.
- `CommunicationManager`'s subscriber lists are copy-on-write; removed
  lists/entries are destroyed *after* the manager's lock is released
  (they may run arbitrary user code). Preserve that ordering in any edit.
- `text_input_provider.cpp` / `window_provider.cpp` look like "empty"
  translation units but are required for MSVC to export the interface's
  constructor/destructor from the DLL — don't delete them as dead code.

## Testing
`tests/` uses the shared hand-rolled `test_framework.hpp` (see
`dsbox_core/tests/main.cpp` for the runner). `tests/fixtures/` builds two
tiny plugin DLLs (`test_plugin`, `test_plugin_incompatible`) used by
`module_loader_tests.cpp` to exercise real dynamic loading and the
version-requirement rejection path.

## Status (this pass)
Added a third `CommunicationManager` endpoint kind, "handled events"
(`registerHandledEvent`/`registerHandledEventScoped`/`unregisterHandledEvent`/
`getHandledEvent`/`publishHandledEvent`/`listHandledEvents` and friends),
to close the gap between a handler (exactly one subscriber, duplicate
registration throws) and a plain event (any number of subscribers, no
answer at all): several independent subscribers can now all answer the
same publish, each getting back its own `HandledEventResult<RT>` (the
value it returned, or the `exception_ptr` it threw) without one
misbehaving subscriber stopping the rest or taking the publisher down.
Implemented by mirroring the existing event machinery (same
copy-on-write subscriber lists, same snapshot-at-the-start `invoke()`
semantics, same debug-event reporting via two new
`comm.debug.handled_event_{un,}subscribed` topics, same participation in
`unregisterOwner()`) rather than generalizing `TypedEventEntry` --
keeping a plain event's hot path untouched was judged more valuable than
the deduplication. `ScopedSubscription` grew a `Kind` tag (`Event` vs.
`HandledEvent`) since the two kinds keep separate id spaces and the id
alone can't say which `unregister*()` to call on `reset()`.

Added a matching test block in `communication_manager_tests.cpp`
(multi-subscriber ordering, per-subscriber exception capture, distinct
return types as distinct endpoints, unregister/last-subscriber-drops-it,
handle invalidation, introspection, `ScopedSubscription`, and
`unregisterOwner`/debug-event coverage). Compiled and run clean
(including under `-fsanitize=address,undefined`) against a staged copy
of the header and test file outside this repo, since no build toolchain
is available from this session directly on the Windows checkout.

Previously surveyed `module_manager`, `module_loader`, `input`,
`window_provider`, `text_input_provider`, `static_registry`,
`version_requirement`, `overridable` with no bugs found; unchanged this
pass.
