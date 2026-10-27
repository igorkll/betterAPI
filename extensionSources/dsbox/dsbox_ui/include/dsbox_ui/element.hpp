#pragma once

#include <dsbox_ui/event.hpp>
#include <dsbox_ui/length.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace dsbox_ui
{
    class IUiRenderBackend;
    class DisplayListBuilder;
    class IUiElementActions;

    // Controls what stays true about an IUiElement's id() across
    // frames. Chosen per-element at creation time (see
    // UiElementCreateOptions below) -- WindowUiTree is the one that
    // actually assigns ids, an element never picks its own, same as
    // dsbox::WindowHandle is assigned by WindowModule rather than by
    // the window itself.
    enum class UiElementIdMode
    {
        // Monotonic counter for the whole WindowUiTree's lifetime.
        // Never reused, even after the element that held it is
        // destroyed -- same guarantee as dsbox::WindowHandle (see
        // WindowModule::Impl::nextHandle). Required for anything that
        // holds onto an id across several frames without also holding
        // a pointer to the element itself: a persistent focus target,
        // a debug inspector's selection, an external reference that
        // should reliably mean "still nothing" (never "now some other
        // element") if the original was removed.
        Unique,

        // Stable for the lifetime of THIS element instance -- doesn't
        // change on move/relayout/repaint -- but is only guaranteed
        // unique among currently-alive elements. Invalidated the
        // moment this instance is destroyed (destructor, or explicit
        // removal from the tree), at which point WindowUiTree may hand
        // the freed slot to a new element. Default for
        // UiElementCreateOptions: cheaper than Unique for trees that
        // frequently rebuild subtrees (conditional rendering), at the
        // cost of not being safe to keep past this element's own life.
        Instance,

        // This element's current position among its parent's children.
        // Changes on ANY reordering/insertion/removal among siblings --
        // valid for at most one pass (e.g. as a hit-test result used
        // and discarded within the same frame). NEVER store this
        // across frames: the same value can silently refer to a
        // different element next frame. Structures that need to
        // outlive a single pass (focus target, dirty-set, any external
        // reference) must reject Index -- see the note on
        // WindowUiTree::setFocus() for where this is enforced.
        Index,
    };

    struct UiElementCreateOptions
    {
        UiElementIdMode idMode = UiElementIdMode::Instance;
        std::string name;
    };

    // Open-ended contract for anything that can appear in a
    // WindowUiTree -- a container, a text run, an image, or (later) a
    // canvas/video/form-control-like element. Deliberately NOT a
    // closed enum of built-in kinds dispatched via switch: a new kind
    // of element is a new class implementing this interface, added
    // without touching dsbox_ui itself. Simple composition (v1's own
    // Container/Text/Image) and future HTML5-like elements are just two
    // different sets of implementations of the same four methods.
    class IUiElement
    {
    public:
        virtual ~IUiElement() = default;

        // Stable identity -- see UiElementIdMode for exactly how
        // stable, which depends on how this element was created.
        virtual UiElementId id() const noexcept = 0;
        virtual UiElementIdMode idMode() const noexcept = 0;

        // Empty unless given a name at creation. No uniqueness
        // guarantee at this layer -- resolving something like HTML's
        // id="..." (unique-by-convention, indexed lookup) is a concern
        // for a layer above IUiElement, which can build a
        // name -> UiElementId index over a tree without this interface
        // having to know that such a thing exists.
        virtual std::string_view name() const noexcept
        {
            return {};
        }

        // Reports this element's own size preference given resolved
        // pixel bounds (see UiConstraints -- units are already gone by
        // this point). A leaf (Text, Image) computes this from its own
        // content, calling into `backend` for anything that needs the
        // active render backend's own facilities (e.g.
        // IUiRenderBackend::measureText() for a Text element). A
        // container returns whatever its own ILayoutStrategy reports
        // after measuring its children -- IUiElement itself has no
        // opinion on how a container arrives at that number.
        virtual UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) = 0;

        // Final box, already fully resolved to window-space pixels by
        // the parent's layout strategy. A container is expected to
        // turn around and arrange() each of its own children inside
        // this rect (typically by handing them straight to its own
        // ILayoutStrategy, which is why `backend` is threaded through
        // here too -- ILayoutStrategy::layout() needs it to measure()
        // children before it can decide their boxes); a leaf just
        // records it for paint()/hit-testing (see bounds() below).
        virtual void arrange(const UiRect& rect, IUiRenderBackend& backend) = 0;

        // The rect passed to the most recent arrange() call, or a
        // zero rect if arrange() has never been called yet. Exists so
        // WindowUiTree can hit-test (walk children, test bounds(),
        // recurse) without every ILayoutStrategy/container having to
        // separately report the same geometry through some other
        // channel -- arrange() is already the one place a final box is
        // decided, so this simply exposes what was last recorded there.
        virtual UiRect bounds() const noexcept = 0;

        // Appends this element's own draw commands (and, for a
        // container, its children's) to `out`. Never mutates layout
        // state -- paint() runs from whatever arrange() last recorded.
        virtual void paint(DisplayListBuilder& out) const = 0;

        // Returns true if this element consumed the event (stops
        // WindowUiTree from continuing the walk further along the
        // current UiEventPhase -- see UiEvent::stopPropagation), false
        // to let it continue to the next ancestor/descendant on the
        // path. Called once per element on the hit-tested path, for
        // every phase (Capture, then Target, then Bubble) -- not just
        // for the element actually hit.
        //
        // `actions` is this element's ONLY channel back to the owning
        // WindowUiTree -- see IUiElementActions below for why it
        // exists and what it's safe to do with it (in particular: only
        // call it from within this very handleEvent() call, never
        // store the reference for later).
        virtual bool handleEvent(const UiEvent& event, IUiElementActions& actions) = 0;

        // Empty span for a leaf. A container returns its actual
        // children; WindowUiTree/layout strategies walk this rather
        // than assuming any particular container implementation.
        virtual std::span<std::unique_ptr<IUiElement>> children() = 0;

        // Called by WindowUiTree, top-down from the root, immediately
        // before a Layout-level measure()/arrange() pass that was NOT
        // triggered by a window resize (see WindowUiTree::tick()'s own
        // "per-subtree scoping" comment). `dirtyIds` is every id
        // markDirty()'d at UiDirtyLevel::Layout since the last pass
        // that actually ran. Returns true if this element's (and its
        // entire subtree's) previous measure()/arrange() results are
        // still valid and may be reused as-is -- false if either this
        // element itself, or something in its subtree, needs to
        // actually recompute.
        //
        // Returning true here is a PROMISE, not just a report -- the
        // caller may skip calling measure()/arrange() on this element
        // entirely based on it. The default implementation makes that
        // promise only when it's trivially safe to: this element's own
        // id() is absent from `dirtyIds` AND it has no children at all
        // (children().empty()) -- i.e. a plain leaf (Text, Image, ...)
        // that nothing specifically markDirty()'d. Any element WITH
        // children must override this to also recurse into each child
        // and OR their results together before it can safely report
        // true -- Container does (see its own override, container.cpp)
        // and is consequently the one built-in element whose subtrees
        // actually get skipped; a composite element that has children
        // but does NOT override this (ScrollView, ListView, Dropdown,
        // as of this writing) falls through to this same default,
        // which sees children().empty() == false and conservatively
        // returns false unconditionally -- always safe, just never
        // skippable, exactly like every element behaved before this
        // method existed.
        virtual bool prepareIncrementalRelayout(const std::unordered_set<UiElementId>& dirtyIds)
        {
            return children().empty() && dirtyIds.find(id()) == dirtyIds.end();
        }

        // Called by WindowUiTree, top-down from the root, before a
        // Layout-level pass that WAS triggered by a window resize --
        // discards whatever prepareIncrementalRelayout() left cached
        // (if anything), forcing this element's (and its subtree's)
        // next measure()/arrange() call to fully recompute regardless.
        // A resize can change ANY element's effective constraints even
        // when nothing about it was ever specifically markDirty()'d (a
        // Percent-sized child resolving differently against a new
        // window size, say), so skipping is never safe here. Default:
        // no-op -- correct for any element that doesn't implement
        // prepareIncrementalRelayout()'s caching in the first place
        // (there's nothing to discard), so only Container overrides
        // this too, purely to reset what ITS OWN override set.
        virtual void forceFullRelayout() {}
    };

    // Narrow set of tree-level actions an element may need to trigger
    // FROM WITHIN its own handleEvent() -- requesting focus for
    // itself (e.g. a click on a focusable element, mirroring HTML's
    // tabindex-on-click), or marking itself dirty after a purely
    // INTERNAL state change (a hover/pressed color, say) that
    // measure()/arrange() never sees and so would otherwise never get
    // repainted. A WindowUiTree implements this and passes itself into
    // every handleEvent() call it makes (see dispatchEvent()/
    // setFocus()) rather than handing an element a raw pointer back to
    // the whole tree -- same reasoning as IUiRenderBackend: expose
    // exactly the handful of calls this side actually needs, nothing
    // else (in particular, no way to read/mutate the tree's structure
    // from here -- that stays exclusively WindowUiTree's own job).
    //
    // Only ever safe to call from within the handleEvent() call that
    // received it -- WindowUiTree only ever hands out a reference to
    // itself for the duration of that one call, on the same thread
    // that's already holding the tree's own (recursive) lock; do not
    // store this reference anywhere for later.
    // What kind of work a markDirty() call actually requires on the
    // next tick() -- see IUiElementActions::markDirty() and
    // WindowUiTree::tick()'s own "two-tier dirty" comment for the full
    // reasoning. Ordered so a future third tier (true per-subtree
    // incremental relayout, see tick()'s own v2 scope note) would
    // slot in without reordering the existing two.
    enum class UiDirtyLevel
    {
        // This element's own paint() output changed, but neither its
        // measure() result NOR anything about where it sits would
        // come out different -- a pressed/hover color swap being the
        // motivating case (Container::handleEvent()'s own
        // PointerDownEvent/PointerEnterEvent/PointerLeaveEvent
        // handlers all use this). tick() can skip measure()/arrange()
        // entirely and still produce a correct frame.
        Repaint,

        // This element's own size and/or its children's arrangement
        // may have changed (setText() on a Text that's about to be
        // markDirty()'d, addChild()/clearChildren(), a changed
        // UiLength, ...) -- tick() must re-run measure()/arrange() for
        // the whole tree (see tick()'s own v2 scope note: TRUE
        // per-subtree-scoped relayout, where only the dirtied
        // element's own ancestors/descendants are re-measured, is
        // still future work -- this tier is "must relayout SOMETHING",
        // not yet "must relayout exactly this much").
        Layout,
    };

    class IUiElementActions
    {
    public:
        virtual ~IUiElementActions() = default;

        // See WindowUiTree::setFocus() for the full contract (id mode
        // requirements, FocusIn/FocusOut semantics) -- this simply
        // forwards to it.
        virtual void requestFocus(UiElementId id) = 0;

        // See WindowUiTree::markDirty(). `level` defaults to Layout --
        // the safe, conservative choice for any caller that hasn't
        // specifically reasoned about whether its own change could
        // possibly affect measure()/arrange() -- so every pre-v2 call
        // site (just `markDirty(m_id)`, no second argument) keeps
        // behaving exactly as before this parameter existed. Pass
        // UiDirtyLevel::Repaint explicitly only when the change is
        // KNOWN to be paint-only, per UiDirtyLevel's own doc.
        virtual void markDirty(UiElementId id, UiDirtyLevel level = UiDirtyLevel::Layout) = 0;

        // Routes every subsequent pointer event (move, up, and any
        // further down) to `id` -- bypassing the normal position-based
        // hit test that would otherwise decide the path -- until that
        // element releases capture (releasePointerCapture()) or a
        // PointerUpEvent has been delivered to it, whichever happens
        // first (see WindowUiTree::dispatchEvent()'s own comment on
        // where that auto-release actually happens). Capture/Target/
        // Bubble still run along `id`'s own ancestor path exactly as
        // for a normally hit-tested event -- only WHICH element the
        // path is built around changes, not how the path is walked.
        //
        // Call this from a PointerDownEvent handler so a press that
        // turns into a drag-and-release outside this element's bounds
        // -- or outside the window entirely -- still reaches THIS
        // element instead of whatever happens to be under the cursor
        // (or nothing at all) at release time. Without it, an element
        // that tracks its own "is the pointer down on me" state (e.g.
        // Container::m_pressed) can get stuck thinking the pointer is
        // still down, since it would simply never see the matching
        // PointerUpEvent.
        virtual void setPointerCapture(UiElementId id) = 0;

        // Ends capture early if `id` currently holds it; a no-op
        // otherwise (including when nothing is currently captured, or
        // `id` isn't the current holder). Most callers never need
        // this -- capture already ends automatically once a
        // PointerUpEvent reaches the capturing element -- it exists for
        // an element that wants to give up capture before that, e.g. a
        // drag gesture cancelling itself.
        virtual void releasePointerCapture(UiElementId id) = 0;
    };

    // Optional capability an IUiElement may additionally implement to
    // take part in keyboard focus traversal (Tab / Shift+Tab -- see
    // WindowUiTree::dispatchEvent()). Queried via dynamic_cast, same
    // pattern and same reasoning as IFlexItem below: most elements
    // (Text, Image, a plain non-focusable Container) have nothing to
    // say here. An element that does NOT implement this is never a
    // traversal stop -- though a direct setFocus() call can still give
    // it focus.
    class IFocusTarget
    {
    public:
        virtual ~IFocusTarget() = default;

        // Tree order (depth-first, children in declaration order) is
        // the traversal order -- there is no tabindex-style override
        // yet. False (e.g. a disabled button) skips this element.
        virtual bool isFocusable() const noexcept = 0;
    };

    // Optional capability for elements that consume typed text (a text
    // field). While the focused element implements this AND
    // acceptsTextInput() is true, WindowUiTree asks its
    // dsbox::ITextInputProvider to start text input for the window --
    // backends only generate TextInputEvent/TextEditingEvent in that
    // state -- and stops it again when focus moves elsewhere.
    class ITextInputTarget
    {
    public:
        virtual ~ITextInputTarget() = default;

        // Checked whenever focus changes; false (a disabled or
        // read-only field) means "don't start text input for me".
        virtual bool acceptsTextInput() const noexcept = 0;

        // Where the platform IME should put its candidate window --
        // typically the caret's box -- in window coordinates. Read by
        // WindowUiTree after each relayout, only while this element
        // has focus.
        virtual UiRect textInputRect() const noexcept = 0;
    };

    // Axis a linear (flex-like) ILayoutStrategy is currently
    // distributing free space along. Exists so an optional capability
    // like IFlexItem (below) can be asked "how flexible are you on
    // THIS axis" without needing to know which concrete
    // ILayoutStrategy is asking -- StackLayout's own StackDirection
    // maps directly onto this (Vertical -> Vertical, Horizontal ->
    // Horizontal); a future grid strategy would use it the same way
    // for whichever axis it treats as primary at a given point.
    enum class UiAxis
    {
        Horizontal,
        Vertical,
    };

    // Optional capability an IUiElement may additionally implement to
    // participate in a linear layout strategy's free-space
    // distribution along whichever axis that strategy treats as
    // "main" -- see StackLayout::layout(). Mirrors UiUnit::Fraction's
    // own documented contract (length.hpp): an element that doesn't
    // implement this (or returns 0 for the relevant axis) is simply
    // not flexible -- exactly the same fallback Fraction's own doc
    // promises for a strategy that doesn't distribute free space at
    // all. Queried via dynamic_cast from the layout strategy rather
    // than folded into IUiElement itself, because most elements (Text,
    // Image, and a Container declared with a Pixel/Percent/Content
    // size) have nothing to say here -- same reasoning as keeping
    // Container-specific configuration off IUiElement in the first
    // place.
    class IFlexItem
    {
    public:
        virtual ~IFlexItem() = default;

        // 0 (the default assumption for anything that ISN'T this
        // interface) means not flexible: this child's own measure()
        // result is used as-is, same as before this interface existed.
        // Positive means this element's main-axis size should grow to
        // fill (weight / sum-of-weights-among-flexible-siblings) of
        // whatever main-axis space is left after every non-flexible
        // sibling has taken its own measure() result and every gap has
        // been subtracted. Mirrors CSS's flex-grow / grid's `fr` in
        // spirit -- not the full algorithm (no shrink, no flex-basis
        // distinct from measure()).
        virtual float flexGrow(UiAxis axis) const noexcept = 0;
    };

    // Resolves each child's box within `contentRect` and calls
    // arrange() on it. Pluggable per-container so a new arrangement
    // algorithm (flex, grid, ...) is a new class implementing this
    // interface, not a rewrite of whatever container class wants to
    // use it -- Container in particular holds one of these rather than
    // hard-coding how it lays out its own children.
    class ILayoutStrategy
    {
    public:
        virtual ~ILayoutStrategy() = default;

        virtual void layout(
            std::span<std::unique_ptr<IUiElement>> children,
            const UiRect& contentRect,
            IUiRenderBackend& backend) = 0;

        // Reports the natural ("fit-content") size this strategy would
        // need to lay out `children` if nothing external were pinning
        // either axis down -- what Container::measure() (container.cpp)
        // asks for on whichever axis has UiUnit::Content (length.hpp).
        // `constraints` is the SAME UiConstraints the asking Container's
        // own measure() received (minus its own padding) -- used here
        // only as an outer clamp (a Content-sized Container still can't
        // exceed whatever bound ITS OWN parent imposed), never as
        // something children are stretched to fill the way layout()'s
        // own contentRect is. Unlike layout(), nothing here is actually
        // arrange()'d -- this may be called from measure(), well before
        // this Container's own final box (and therefore its children's)
        // is known.
        //
        // v1 limitation (StackLayout's own implementation): a child that
        // implements IFlexItem and reports a positive flexGrow() for
        // this strategy's main axis (see layout()'s own "Free-space
        // distribution" comment) has no "leftover space" to claim here
        // -- there is no fixed main extent yet, that's the whole point
        // of asking for Content sizing in the first place. Such a
        // child's own measure() result is used exactly as any
        // non-flexible child's would be instead.
        virtual UiSize measureContent(
            std::span<std::unique_ptr<IUiElement>> children,
            const UiConstraints& constraints,
            IUiRenderBackend& backend) = 0;
    };
}
