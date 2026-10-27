#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/length.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dsbox_ui
{
    // Simple container element: a solid (optionally rounded-corner)
    // background rect, padding, and a set of children arranged by a
    // pluggable ILayoutStrategy (see stack_layout.hpp for the first
    // one). This is v1's one general-purpose container -- the future
    // HTML5-like elements discussed alongside dsbox_ui's design are
    // meant to arrive as sibling IUiElement implementations, not as
    // extensions bolted onto this class.
    //
    // v1 sizing limitation: measure() resolves this Container's OWN
    // UiLength width/height (see setPreferredWidth()/Height()) directly
    // against the incoming UiConstraints -- Pixel and Percent both
    // work correctly, but Auto/Content/Fraction do NOT yet size this
    // Container to fit its children's natural size (that needs a real
    // two-pass measure, which no ILayoutStrategy implementation does
    // yet -- see StackLayout's own comment). They currently just fill
    // the incoming max constraint (or fall back to min, if the max is
    // unbounded). Give every Container an explicit Pixel or Percent
    // size for now.
    //
    // v1 identity limitation: unlike the "WindowUiTree mints ids"
    // model described when UiElementIdMode was designed, this
    // constructor takes an id supplied by the CALLER -- there is no
    // id-minting factory/builder yet. Keeping ids unique is the
    // caller's own responsibility for now.
    class DSBOX_UI_API Container final : public IUiElement, public IFlexItem, public IFocusTarget
    {
    public:
        explicit Container(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~Container() override;

        Container(const Container&) = delete;
        Container& operator=(const Container&) = delete;

        // -- IUiElement --
        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_idMode; }
        std::string_view name() const noexcept override { return m_name; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) override;
        void arrange(const UiRect& rect, IUiRenderBackend& backend) override;
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder& out) const override;

        // Reacts to pointer/focus interaction targeted AT THIS
        // element specifically -- see the "-- Interactivity --"
        // section below for what that means and how to opt in. A
        // Container with none of that configured behaves exactly as
        // before this existed: always returns false (never consumes),
        // letting every event continue toward whichever
        // descendant/ancestor actually handles it.
        bool handleEvent(const UiEvent& event, IUiElementActions& actions) override;

        std::span<std::unique_ptr<IUiElement>> children() override;

        // Real caching override -- see IUiElement::
        // prepareIncrementalRelayout()'s own doc (element.hpp) for the
        // general contract this must satisfy. Checks m_id against
        // `dirtyIds`, then recurses into EVERY child regardless of
        // whether an earlier child already proved this subtree dirty
        // (each child's own cached state must be refreshed every call,
        // not just the first one that happens to report true), ORing
        // every result together into m_relayoutDirty -- which measure()/
        // arrange() (container.cpp) then consult to decide whether to
        // skip their own recomputation entirely.
        bool prepareIncrementalRelayout(const std::unordered_set<UiElementId>& dirtyIds) override;

        // Forces m_relayoutDirty true and recurses into every child's
        // own forceFullRelayout() -- see IUiElement's own doc
        // (element.hpp) for why a resize needs this instead of
        // prepareIncrementalRelayout().
        void forceFullRelayout() override;

        // -- IFlexItem --

        // Returns m_preferredWidth/Height's own value when that axis's
        // UiLength is UiUnit::Fraction, 0 otherwise -- see IFlexItem's
        // own doc (element.hpp) for what a layout strategy does with
        // this. Declaring a Container's width or height as
        // UiLength::fraction(weight) is what actually opts it into a
        // StackLayout's free-space distribution; nothing else needed.
        float flexGrow(UiAxis axis) const noexcept override;

        // -- Container-specific configuration --

        // Transparent (alpha 0) by default -- a Container with no color
        // set draws nothing of its own, only whatever its children
        // paint.
        void setBackgroundColor(UiColor color) { m_backgroundColor = color; }
        void setCornerRadius(float radius) { m_cornerRadius = radius; }
        void setPadding(float top, float right, float bottom, float left);

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

        // Wraps this Container's children's paint() output in a
        // PushClip/PopClip pair scoped to this Container's own bounds.
        // Off by default.
        void setClipsChildren(bool clip) { m_clipsChildren = clip; }

        // CSS3 opacity, applied to this Container's OWN fill only --
        // 1.0 (the default) means "no change" (paint() behaves exactly
        // as before this existed), 0.0 means this Container's
        // background (and pressed-color override) paints fully
        // transparent. Values are clamped to [0, 1]; anything outside
        // that range is simply clamped rather than rejected.
        //
        // v1 scope limitation: unlike CSS's own opacity (which composites
        // an element and its entire subtree as one group, so a 50%-
        // opaque parent also visually fades every child), this only
        // ever scales THIS Container's own RectCommand alpha -- it does
        // NOT propagate to children's paint() calls. A true group-
        // opacity implementation needs the display list / backend to
        // support rendering a subtree to an offscreen target and
        // compositing that as a whole (see DisplayListBuilder's own
        // push/popClip for the closest existing precedent, which has
        // the same single-rect scope problem); that's a render_backend.hpp
        // / DisplayList-level feature, not something Container can
        // fake on its own. A Container with only itself to paint (no
        // children, or children that don't need to fade too) is
        // unaffected by this limitation.
        void setOpacity(float opacity) { m_opacity = std::clamp(opacity, 0.0f, 1.0f); }
        float opacity() const noexcept { return m_opacity; }

        // -- Interactivity --
        //
        // Mirrors the HTML mental model this whole design leans on
        // (see the Project brief): any element can carry a click
        // handler and opt into being a focus/tab target, not just
        // some separate "Button" kind -- a plain <div onclick
        // tabindex="0"> is a completely ordinary thing to write in
        // HTML, and a Container configured this way is the equivalent
        // here. Nothing below is required -- an unconfigured Container
        // is inert exactly as before this existed.
        //
        // All of this fires from handleEvent() being called at
        // UiEventPhase::Target (this element is itself the hit/
        // focused element) OR at UiEventPhase::Bubble (some
        // non-interactive descendant -- e.g. a Button's label Text,
        // see button.cpp's makeButton() -- was the actual hit element
        // but declined to handle the event, and it is now bubbling
        // back up through this Container). It deliberately does NOT
        // fire during UiEventPhase::Capture, when this element is only
        // being passed through on the way DOWN toward whatever the
        // real hit element turns out to be. event.target is NOT
        // required to equal this element's own id() -- only phase is
        // checked -- precisely so a click that lands on a non-
        // interactive child (a label, an icon) still counts as a
        // click on the interactive Container that contains it, same
        // as a <button>'s text node in HTML.
        //
        // At Bubble specifically, a Container that is neither
        // focusable nor has setOnClick() called on it stays completely
        // transparent to the event instead -- a plain, unconfigured
        // wrapper Container (pure layout, the equivalent of a <div>
        // with neither onclick nor tabindex) must never swallow a
        // click meant for an interactive descendant or ancestor just
        // by virtue of sitting on the path between them. Only an
        // ACTUALLY interactive Container (setFocusable(true) and/or
        // setOnClick() called) claims a bubbled event; Target phase is
        // unaffected by this -- a Container directly hit still tracks
        // isPressed() regardless of whether it's otherwise configured,
        // same as before Bubble handling existed at all.

        // False by default. A focusable Container calls
        // IUiElementActions::requestFocus() on itself when it receives
        // a PointerDownEvent targeted at it -- click-to-focus, same as
        // a focusable HTML element. Also makes this Container a Tab/
        // Shift+Tab traversal stop (see IFocusTarget, element.hpp, and
        // WindowUiTree::dispatchEvent()'s own Tab handling) in tree
        // order among every other focusable element.
        void setFocusable(bool focusable) { m_focusable = focusable; }

        // -- IFocusTarget --
        bool isFocusable() const noexcept override { return m_focusable; }

        // Fires on PointerUpEvent, but ONLY if THIS element also saw
        // the matching PointerDownEvent first (press-then-release-on-
        // the-same-element, same as a real click) -- a PointerUpEvent
        // that arrives without a preceding PointerDownEvent targeted
        // here (e.g. the button was pressed down over a sibling and
        // dragged onto this one) does NOT fire this. Where the release
        // actually happened is not considered -- same as before pointer
        // capture existed -- only whether THIS element is the one that
        // receives it.
        //
        // A PointerDownEvent targeted here captures the pointer (see
        // IUiElementActions::setPointerCapture()) for the rest of the
        // gesture, so the matching PointerUpEvent is now GUARANTEED to
        // reach this element -- including a release that happens after
        // the cursor left this element's bounds, or the window
        // entirely -- rather than potentially never arriving at all.
        // Previously that case left isPressed() stuck true forever,
        // since this element would simply never see the up event that
        // was supposed to clear it; see dsbox_sdl's SDL_CaptureMouse()
        // fix for the matching OS-level half of that same failure mode.
        void setOnClick(std::function<void()> onClick) { m_onClick = std::move(onClick); }

        // Runtime state, not configuration -- true from the moment a
        // PointerDownEvent targeted at this element is seen until the
        // matching PointerUpEvent (see setOnClick()'s own note on what
        // "matching" means here). Exposed so an application can read
        // it (e.g. from a custom paint()-adjacent decision), on top of
        // the built-in setPressedColor() below.
        bool isPressed() const noexcept { return m_pressed; }

        // Alpha 0 (the default) means "no override": paint() always
        // uses setBackgroundColor()'s color regardless of isPressed().
        // A non-transparent pressedColor is drawn INSTEAD of the
        // regular background color for as long as isPressed() is true
        // -- the closest thing dsbox_ui has right now to CSS's
        // :active pseudo-class. There is deliberately no equivalent
        // for :hover yet -- that needs WindowUiTree to synthesize
        // pointer-enter/leave from consecutive PointerMoveEvents
        // against a remembered "previously hit" path, which does not
        // exist yet (PointerMoveEvent itself is the only motion event
        // in event.hpp's UiEventPayload; there is no
        // PointerEnterEvent/PointerLeaveEvent to react to here even if
        // Container wanted to).
        void setPressedColor(UiColor color) { m_pressedColor = color; }

        // Alpha 0 (the default) means no override, same convention as
        // setPressedColor(). A non-transparent hoverColor is drawn
        // INSTEAD of the regular background color (see paint() for the
        // exact precedence against setPressedColor()'s own override --
        // :active still beats :hover, matching ordinary CSS cascade
        // behavior) for as long as isHovered() is true -- dsbox_ui's
        // :hover equivalent, now that WindowUiTree synthesizes
        // PointerEnterEvent/PointerLeaveEvent (event.hpp) from
        // consecutive pointer-position events. Unlike setPressedColor(),
        // this needs no pointer-capture-style plumbing of its own --
        // WindowUiTree's updateHoverState() (window_ui_tree.cpp)
        // already guarantees a Leave for every Enter.
        void setHoverColor(UiColor color) { m_hoverColor = color; }

        // Runtime state, not configuration -- true from this
        // Container's own PointerEnterEvent until the matching
        // PointerLeaveEvent. Exposed for the same reason isPressed()
        // is -- see that method's own doc.
        bool isHovered() const noexcept { return m_hovered; }

        // nullptr clears any strategy -- children then simply stop
        // being (re)arranged; each keeps whatever box, if any, a
        // PREVIOUS strategy last gave it.
        void setLayoutStrategy(std::unique_ptr<ILayoutStrategy> strategy);

        // Returns a reference to the appended child, for convenient
        // further configuration at the call site (see the demo app
        // for the intended chaining style).
        IUiElement& addChild(std::unique_ptr<IUiElement> child);

        // Destroys every current child, leaving this Container with
        // none -- the structural counterpart to addChild() that v1
        // otherwise lacks (no removeChild() for a single child; this
        // covers the common case that actually comes up: rebuilding a
        // whole subtree from scratch). Intended for a host that swaps
        // what's mounted under one long-lived Container -- a tabbed or
        // panel-switching UI being the motivating case (see
        // dsbox_ui::PanelRegistry, panel.hpp) -- without tearing down
        // and recreating the entire WindowUiTree just to change one
        // branch of it. Mirrors the framework's own stated rationale
        // for UiElementIdMode::Instance ("cheaper... for trees that
        // frequently rebuild subtrees"): this is that rebuild,
        // expressed as an explicit operation instead of a full
        // WindowUiTree replacement.
        //
        // Like any other structural change (addChild(), setLayoutStrategy()),
        // this does not itself trigger a relayout or repaint -- the
        // caller must still call WindowUiTree::markDirty(id()) (or
        // IUiElementActions::markDirty() from inside a handleEvent())
        // afterward. It also does not itself synchronize against a
        // concurrent tick()/dispatchEvent() on another thread -- same
        // as addChild() -- so a caller mutating a tree from a thread
        // other than whichever one calls WindowUiTree::tick()/
        // dispatchEvent() for it must arrange its own serialization
        // (e.g. by only ever mutating from that same thread).
        void clearChildren() { m_children.clear(); }

    private:
        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        UiColor m_backgroundColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        float m_cornerRadius = 0.0f;
        float m_paddingTop = 0.0f, m_paddingRight = 0.0f, m_paddingBottom = 0.0f, m_paddingLeft = 0.0f;
        bool m_clipsChildren = false;

        UiLength m_preferredWidth = UiLength::autoLength();
        UiLength m_preferredHeight = UiLength::autoLength();

        bool m_focusable = false;
        std::function<void()> m_onClick;
        bool m_pressed = false;
        UiColor m_pressedColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        bool m_hovered = false;
        UiColor m_hoverColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        float m_opacity = 1.0f;

        std::unique_ptr<ILayoutStrategy> m_layoutStrategy;
        std::vector<std::unique_ptr<IUiElement>> m_children;

        UiRect m_bounds;

        // -- Incremental relayout caching (see prepareIncrementalRelayout()'s
        // own doc above and IUiElement's, element.hpp) --

        // Starts true so the very first measure()/arrange() call on a
        // freshly constructed Container always fully computes (there is
        // nothing cached yet to reuse) -- same reasoning
        // WindowUiTree::m_dirty/m_needsRelayout start true for the very
        // first tick(). Set by prepareIncrementalRelayout()/
        // forceFullRelayout(); read (and left otherwise untouched) by
        // measure()/arrange().
        bool m_relayoutDirty = true;

        // Only meaningful once this Container has actually measured at
        // least once (guaranteed by m_relayoutDirty starting true,
        // above, which forces that first real measure() call before
        // either of these could ever be consulted).
        UiConstraints m_lastMeasureConstraints{};
        UiSize m_lastMeasureSize{};
    };
}
