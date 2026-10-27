# dsbox_ui — Agent Notes

## Purpose
Retained-mode UI toolkit: an `IUiElement` tree per window
(`WindowUiTree`), a small set of built-in elements (Container, Text,
Image, Button (built on Container), Dropdown, ListView, ScrollView,
TextField, TextArea, ProgressBar, StackLayout), and `UiModule`, which
subscribes to `dsbox_core`'s raw input bus events and translates them
into hit-tested, capture/target/bubble `UiEvent`s per window. Entirely
backend-agnostic: it depends on `dsbox::IWindowProvider`/
`ITextInputProvider` (both `dsbox_core` interfaces), never on
`dsbox_sdl` directly — `dsbox_ui` is usable with any windowing backend
that implements those two interfaces.

## Key types
- `IUiElement` (`element.hpp`) — open-ended per-element contract
  (measure/arrange/paint/handleEvent/children), deliberately not a
  closed enum dispatched via switch.
- `IUiElementActions` (`element.hpp`) — the narrow callback interface an
  element gets during its own `handleEvent()` to talk back to its owning
  `WindowUiTree`: `requestFocus()`, `markDirty()`, and (new this pass)
  `setPointerCapture()`/`releasePointerCapture()`.
- `WindowUiTree` (`window_ui_tree.hpp/cpp`) — owns one window's tree,
  dispatches events (hit-test or focus-target → capture/target/bubble
  walk), runs `tick()` (measure/arrange/paint), and implements
  `IUiElementActions`. `m_treeMutex` is a `std::recursive_mutex`
  specifically because an element's `handleEvent()` can call back into
  the tree (`requestFocus`, `markDirty`, now also
  `setPointerCapture`/`releasePointerCapture`) from inside the very
  dispatch call that invoked it.
- `Container` (`container.hpp/cpp`) — the one general-purpose element;
  `makeButton()` (`button.cpp`) is just a pre-configured `Container` +
  `Text` child, not a separate class.

## V3 pass (current): PointerCancelEvent, multi-line text wrap, true per-subtree incremental relayout

Three features closing out the "Not done" list the V2 pass below left
behind. All three were requested together explicitly, including the
riskiest one (incremental relayout) that would otherwise have been
deferred on its own merits.

**`PointerCancelEvent`** (`event.hpp`) -- the gap V2's own "Known-subtle
 areas" called out ("v1 has no `PointerCancelEvent`"). Synthesized and
dispatched (`UiEventPhase::Target` only -- never hit-tested or bubbled,
same as `PointerEnterEvent`/`PointerLeaveEvent`) by a new
`WindowUiTree::cancelPointerCapture()` (`window_ui_tree.cpp`), which
clears `m_pointerCaptureTarget` BEFORE dispatching (so a handler that
turns around and calls `setPointerCapture()`/`releasePointerCapture()`
again isn't fighting its own in-flight cancel). Wired into two existing
entry points rather than adding new public API surface:
`releasePointerCapture(id)` now calls `cancelPointerCapture()` instead
of clearing capture directly (an explicit, non-auto-release end to a
gesture), and `setFocus()` calls it unconditionally before anything
else (a focus change mid-press is as good a reason as any for the
gesture to not complete normally). `Container::handleEvent()` reacts
to it by clearing `m_pressed` and marking `Repaint`-dirty, same
visual effect a normal release would have, just without firing
`onClick`. Regression tests:
`ReleasePointerCaptureFiresPointerCancelEvent`,
`SetFocusCancelsActivePointerCapture` in
`tests/window_ui_tree_tests.cpp`.

**Multi-line text wrap** (`Text`, `text.hpp/cpp`) -- opt-in via
`setWrapEnabled(bool)` (default `false`, preserving the exact old
single-line behavior). When enabled AND the incoming
`UiConstraints::maxWidth` is finite, `measure()` greedily word-wraps:
splits `m_text` on ASCII spaces only (no Unicode line-breaking, no
`\n` handling, no hyphenation -- all deliberate, documented
limitations, see `text.hpp`'s own "v3 layout addition" comment), packs
words onto a line until the next one would overflow `maxWidth`, never
breaks a single overlong word mid-word (it just overflows alone on its
own line). `m_shapedGlyphs` (one run) became `m_shapedLines` (one run
per line); `paint()` now loops over it, stacking each line's baseline
`lineHeight` below the last. Every line is assumed to share the first
line's font-level metrics, since `TextMetrics` conflates font-level and
content-level data in this API. Tests:
`WrapDisabledByDefaultIgnoresMaxWidth`,
`WrapEnabledWithUnboundedWidthStaysSingleLine`,
`WrapEnabledGreedilyPacksWordsAndReportsStackedHeight`,
`WrapEnabledNeverBreaksAWordWiderThanMaxWidth` in `tests/text_tests.cpp`.

**True per-subtree incremental relayout** -- the piece V2's "two-tier
dirty" explicitly left undone ("Explicitly NOT done: scoping either
tier to the specific dirtied element's own subtree"). Two new
`IUiElement` virtuals (`element.hpp`), both with safe non-pure
defaults: `prepareIncrementalRelayout(const std::unordered_set<UiElementId>&
dirtyIds)` -- returns whether this subtree can be skipped this pass
("promise, not just report": a caching element must actually refresh
its own cached-dirty flag as a SIDE EFFECT of this call, since
`measure()`/`arrange()` consult that flag afterward, not the return
value) -- and `forceFullRelayout()`, which unconditionally discards any
cached state tree-wide. The default `prepareIncrementalRelayout()` is
`children().empty() && dirtyIds.find(id()) == dirtyIds.end()`: correct
unconditionally for any LEAF (nothing to recurse into, so "am I
myself in the dirty set" is the whole answer), and conservative-but-
safe for anything with children that doesn't override it (reports
"always dirty" -- `ScrollView`/`ListView`/`Dropdown` are the three
existing composite elements that don't opt in yet, so a tree built out
of them doesn't benefit, but nothing about them is incorrect either).
`Container` is the one built-in override
(`container.hpp`/`container.cpp`): recurses into every child
unconditionally (each child's own cached state must refresh every
call, not just until the first one reports dirty), ORs the results
together into `m_relayoutDirty`. `Container::measure()` gained a cache
check at its very top (`if (!m_relayoutDirty && constraintsEqual(...))
return m_lastMeasureSize;`) and `Container::arrange()` gained the
analogous one (`if (!m_relayoutDirty && rectUnchanged) return;` --
requires BOTH, since a clean subtree can still need to physically MOVE
if an earlier sibling grew/shrank; the parent's own layout strategy
still calls `arrange()` on every child regardless, only the RECURSION
inside a clean child's own `arrange()` is what gets skipped).
`WindowUiTree::m_dirtyElements` (a new `std::unordered_set<UiElementId>`)
accumulates every id `markDirty()`'d at `UiDirtyLevel::Layout` since the
last committed pass; `tick()` hands it to
`m_root->prepareIncrementalRelayout()` before measuring -- UNLESS the
window size itself changed since last tick, in which case
`m_root->forceFullRelayout()` runs instead (a resize can change ANY
element's effective constraints, e.g. a `Percent`-sized child, even
without a single `markDirty()` call anywhere). Regression test:
`IncrementalRelayoutSkipsUntouchedContainerSubtree` in
`tests/container_tests.cpp` -- proves a sibling `Container` subtree
with nothing dirtied inside it has its `measure()` pass skipped
entirely (down to its own leaf child, via a `measureCount`-tracking
test double), even though the shared parent above both subtrees was
still forced to re-run its own pass because the OTHER child was dirty.

## V2 pass (two passes ago): hover, content-sizing, two-tier dirty

Three features, each scoped deliberately short of the "real" version of
itself -- see each one's own doc for exactly where the line was drawn
and why:

**Hover / :hover** -- `PointerEnterEvent`/`PointerLeaveEvent` (event.hpp),
synthesized by `WindowUiTree::updateHoverState()` (window_ui_tree.cpp)
from every pointer-position event's own hit-tested/capture-routed path,
diffed against `m_hoveredPath` (ids, not raw pointers -- the tree may
have been mutated between events). Dispatched directly at
`UiEventPhase::Target` to exactly the elements whose hover state
changed (Leave deepest-first, Enter shallow-to-deep -- mirrors HTML's
non-bubbling mouseenter/mouseleave, not mouseover/mouseout), never
bubbled. `Container::setHoverColor()`/`isHovered()` (container.hpp) is
the one built-in consumer so far; precedence in `paint()` is pressed >
hover > background, matching ordinary CSS `:active`/`:hover` cascade
order. While pointer capture is active, hover simply follows the
captured element's own path rather than the raw cursor position.

**Real content-sizing (`UiUnit::Content`)** -- `ILayoutStrategy` gained
`measureContent()` (element.hpp): the actual two-pass measure
`Container`'s own long-standing "v1 sizing limitation" comment called
out as missing. `Container::measure()` (container.cpp) calls it when
either axis is `UiUnit::Content`, asking the layout strategy what its
children naturally need (minus this Container's own padding) instead
of resolving `m_preferredWidth`/`Height` directly.
`StackLayout::measureContent()` (stack_layout.cpp) is the one
implementation so far -- sums main-axis sizes + gaps, maxes cross-axis
size, same as `layout()`'s own Pass 1 minus the actual `arrange()`
calls. Known gap: a flexible (`IFlexItem`) child has no "leftover
space" to claim here, since there's no fixed main extent yet -- it's
measured like any non-flexible child instead.

**Two-tier dirty (NOT yet true per-subtree incremental relayout)** --
`UiDirtyLevel` (element.hpp): `Repaint` (paint() only, e.g. a pressed/
hover color swap) vs `Layout` (the old, only, behavior -- re-measure/
re-arrange the WHOLE tree). `IUiElementActions::markDirty()` gained a
second parameter defaulting to `Layout`, so every pre-v2 call site
(bare `markDirty(m_id)`) keeps behaving exactly as before.
`WindowUiTree::tick()` tracks `m_needsRelayout` separately from
`m_dirty` and skips `measure()`/`arrange()` entirely when nothing
Layout-level happened since the last committed pass.
`Container::handleEvent()`'s pressed/hover branches all pass
`UiDirtyLevel::Repaint` now. Explicitly NOT done: scoping either tier
to the specific dirtied element's own subtree -- `markDirty()` still
takes a `target` id it doesn't yet act on positionally; a Layout-level
change anywhere still re-measures/re-arranges everything, just no
longer on EVERY markDirty() call regardless of level.

### A guard the hover/content work surfaced: non-interactive Containers must stay transparent at Bubble

Enabling `Container::handleEvent()` to react at `UiEventPhase::Bubble`
at all (see the click-on-label fix below) would, without a further
check, make EVERY Container ancestor on a bubble path swallow a
PointerDownEvent/PointerUpEvent just by being a Container -- including
a plain, unconfigured layout wrapper with no `onClick` and not
focusable. Fixed by gating Bubble-phase pointer handling on
`m_focusable || m_onClick` (`container.cpp`): only an ACTUALLY
interactive Container claims a bubbled event; Target phase (this
Container directly hit) is unaffected and still tracks `isPressed()`
unconditionally, same as always. Regression test:
`NonInteractiveWrapperDoesNotSwallowBubbledEvent` in
`tests/container_tests.cpp`.

## Bug fixed (three passes ago)
**A click that hit-tested onto a non-interactive descendant -- most
notably a `Button`'s own label `Text` (see `button.cpp`'s `makeButton()`)
-- did not fire the Container's `onClick`.** `Container::handleEvent()`
only ever reacted when `event.phase == UiEventPhase::Target` AND
`event.target == m_id`. `Text::handleEvent()` always returns `false`
and never changes `event.target`, so a `PointerDownEvent`/
`PointerUpEvent` hit-tested onto a label reached the owning `Container`
only during `UiEventPhase::Bubble`, with `event.target` still equal to
the *label's* id -- which the old gate silently dropped. In practice:
clicking the bare background of a button worked, clicking its own text
did nothing.

Fix: `Container::handleEvent()` now reacts at both `UiEventPhase::Target`
(this element is itself the hit/focused element) and
`UiEventPhase::Bubble` (a descendant was hit but didn't consume the
event, and it's bubbling back up through this Container) -- only
`UiEventPhase::Capture` is excluded. `event.target` is no longer
checked at all; only phase is. See `container.hpp`'s own
"-- Interactivity --" section and `container.cpp`'s `handleEvent()` for
the full reasoning.

Regression test: `ClickOnLabelGlyphsStillFiresButtonOnClick` in
`tests/button_tests.cpp` (gives the label real, non-zero `TextMetrics`
so it actually occupies hit-testable space, then clicks squarely inside
the label's own glyph bounds). The existing
`MakeButtonRootFiresOnClickLikeAnyOtherContainer` test did NOT catch
this: its `DummyRenderBackend` reports all-zero `TextMetrics`, so the
label has zero size and the click there could only ever land on the
button's own background.

## Also this pass: `Container::setOpacity()`
A CSS3-`opacity`-flavored addition to `Container`'s styling surface:
`setOpacity(float)`/`opacity()`, clamped to `[0, 1]`, scales this
Container's own fill alpha (background or pressed-color override) in
`paint()`. Scope is deliberately narrow -- see `setOpacity()`'s own doc
in `container.hpp` -- it does **not** composite this Container and its
subtree as one faded group the way CSS's own `opacity` does; that needs
offscreen-target support at the `DisplayList`/`render_backend.hpp`
layer, which v1 doesn't have (same limitation `setClipsChildren()`'s
single-rect `PushClip`/`PopClip` already has, for the same underlying
reason). Tests: `OpacityScalesFillAlpha`, `OpacityIsClampedToUnitRange`
in `tests/container_tests.cpp`.

## Bug fixed (four passes ago)
**A `Container`'s pressed/active state (`m_pressed`, and therefore
`setPressedColor()`'s visual effect and `isPressed()`) could get stuck
true forever** if the matching `PointerUpEvent` never reached that same
element — which happened whenever a press turned into a drag that ended
outside the element's bounds (hit-tests to a different element or
nothing), or outside the window entirely (see `dsbox_sdl`'s own
`Agent.md` for the OS-level half of that second case).

Fix: added pointer capture to `IUiElementActions`/`WindowUiTree`.
`Container::handleEvent()` now calls `actions.setPointerCapture(m_id)`
from its `PointerDownEvent` handler; while captured, `WindowUiTree::
dispatchEvent()` routes *every* subsequent pointer event (move, up, and
any further down) straight to the capturing element's own ancestor path
— bypassing the normal position-based hit test entirely — until that
element actually receives a `PointerUpEvent` (auto-release) or calls
`releasePointerCapture()` itself. Capture does **not** change what
`Container::setOnClick()`'s handler does once the up event arrives —
same "any up after a matching down fires the click, position not
considered" semantics as before; it only guarantees that up event is no
longer sometimes lost.

Regression test: `testPointerCaptureRoutesEventsToCapturedElementRegardlessOfPosition`
in `tests/window_ui_tree_tests.cpp` (tree-level: capture holds through
out-of-bounds move/up, then auto-releases). `tests/container_tests.cpp`'s
existing click tests were re-verified against this change — they still
pass because click-firing was deliberately left position-independent
(see above); do not add a "release must land inside bounds()" gate to
`Container::handleEvent()` without also updating every test that
dispatches a `PointerUpEvent` against a `Container` whose `m_bounds` was
never set via `arrange()`/`tick()` (several existing tests construct a
`Container` and dispatch events without ever calling `tick()` first, so
`m_bounds` stays a zero-size default rect).

## Known-subtle areas
- Only `Container` tracks persistent press state today (confirmed by
  reading every other interactive element this pass: `Dropdown`,
  `ListView`, `TextField` all act immediately on `PointerDownEvent`
  rather than deferring to the matching `PointerUpEvent`, so they have
  no equivalent stuck-state hazard and don't call
  `setPointerCapture()`). A *new* element that defers an action to
  `PointerUpEvent` after `PointerDownEvent` should capture the pointer
  the same way `Container` does.
- `WindowUiTree::dispatchEvent()`'s capture branch still walks
  Capture → Target → Bubble along the captured element's own ancestor
  path (via `findPath()`), exactly like a normal hit-tested dispatch —
  capture only changes *which* element the path is built around, not
  how the path is walked once built.
- `PointerCancelEvent` now exists (V3 pass, above) but only fires from
  the two places `cancelPointerCapture()` is wired into
  (`releasePointerCapture()` and `setFocus()`) — anything else that
  wants a gesture to end early without a matching `PointerUpEvent`
  still needs to call `releasePointerCapture()` itself to get it.

## Testing
Per-element test files under `tests/`, plus `window_ui_tree_tests.cpp`
for dispatch/focus/tick mechanics. Shared `test_framework.hpp` runner.
