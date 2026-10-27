#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/length.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dsbox_ui
{
    // Single-child scroll/clipping container: a fixed-size viewport
    // (this element's own measure()/arrange() box) over one child that
    // may be measured larger than the viewport on either axis,
    // scrolled by an internal offset, and always clipped to the
    // viewport's own bounds. The HTML equivalent is a block element
    // with `overflow: auto` -- ScrollView is dsbox_ui's one way to get
    // that, the same role Container's own setClipsChildren() plays for
    // "clip, but never scroll" (container.hpp).
    //
    // How scrolling actually works, given this toolkit's display list
    // has no translate/transform command (display_list.hpp): the child
    // is measured ONCE against unbounded constraints on whichever
    // axis/axes are scrollable (see measure()), then arrange()d at an
    // ABSOLUTE position shifted by the current scroll offset --
    // typically partly or wholly outside the viewport's own bounds --
    // exactly the way every other element positions a child via
    // arrange(); paint() then wraps the child's own paint() output in
    // a PushClip/PopClip pair scoped to the viewport, the same
    // mechanism Container::setClipsChildren() already uses. Scrolling
    // is therefore nothing more than re-arranging the child at a
    // different absolute position and letting the clip do its job --
    // no new DisplayList primitive was needed.
    //
    // v1 limitations:
    //  - Hit-testing (WindowUiTree's own hitTestPath(), window_ui_tree.cpp)
    //    walks purely by bounds().contains(point) -- it has no notion
    //    of an ancestor's clip rect. A child (or grandchild) currently
    //    scrolled OUTSIDE the visible viewport can therefore still be
    //    hit-tested and receive pointer events meant for whatever is
    //    actually visible underneath/around it. This is a limitation
    //    of WindowUiTree's hit-testing in general (nothing in dsbox_ui
    //    clips hit-testing to a visible region today), not something
    //    ScrollView can fix on its own -- a real fix needs
    //    WindowUiTree's own hitTestPath() to consult each ancestor's
    //    clip state, which does not exist yet.
    //  - Mouse-wheel scrolling only works while this ScrollView (or
    //    nothing) HAS FOCUS -- see PointerWheelEvent's own doc
    //    (event.hpp): unlike Pointer{Down,Up,Move}Event, a wheel event
    //    is never hit-tested by position (WindowUiTree's own
    //    isPointerPayload() does not include it), so it is routed the
    //    same way a keyboard event is: to whatever currently holds
    //    focus. Make a ScrollView focusable (it already implements
    //    IFocusTarget) and request focus for it (e.g. on
    //    PointerDownEvent, which IS hit-tested) if wheel scrolling
    //    without first clicking inside it matters to a particular UI.
    //  - One child only -- wrap a Container (with its own
    //    ILayoutStrategy) as that one child to scroll a whole laid-out
    //    subtree, the same pattern HTML uses (overflow is a property of
    //    one element, whose own children can be arbitrarily complex).
    //  - The non-scrollable axis stretches the child to exactly fill
    //    the viewport on that axis (min 0, max viewport extent) --
    //    there is no "scroll on neither axis but still clip" mode
    //    distinct from Container::setClipsChildren() for that.
    //  - Scrollbars are a fixed-width overlay indicator, not an
    //    interactive drag target -- see setShowScrollbars()'s own doc.
    class DSBOX_UI_API ScrollView final : public IUiElement, public IFocusTarget
    {
    public:
        explicit ScrollView(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~ScrollView() override;

        ScrollView(const ScrollView&) = delete;
        ScrollView& operator=(const ScrollView&) = delete;

        // -- IUiElement --
        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_idMode; }
        std::string_view name() const noexcept override { return m_name; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) override;
        void arrange(const UiRect& rect, IUiRenderBackend& backend) override;
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder& out) const override;
        bool handleEvent(const UiEvent& event, IUiElementActions& actions) override;
        std::span<std::unique_ptr<IUiElement>> children() override;

        // -- IFocusTarget --
        bool isFocusable() const noexcept override { return m_focusable; }

        // -- Configuration --

        // Replaces the current child (if any) and returns a reference
        // to it, same "convenient further configuration at the call
        // site" convention Container::addChild() uses (container.hpp).
        IUiElement& setChild(std::unique_ptr<IUiElement> child);
        void clearChild() { m_children.clear(); }
        IUiElement* child() noexcept { return m_children.empty() ? nullptr : m_children.front().get(); }

        // Both default true/false respectively: vertical scrolling on,
        // horizontal off -- the common "a tall list in a fixed-height
        // panel" case. Disabling an axis also stretches the child to
        // exactly fill the viewport on that axis (see this class's own
        // v1 note above) rather than merely clamping scroll on it to 0.
        void setVerticalScrollEnabled(bool enabled) { m_verticalScroll = enabled; }
        void setHorizontalScrollEnabled(bool enabled) { m_horizontalScroll = enabled; }

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

        void setBackgroundColor(UiColor color) { m_backgroundColor = color; }
        void setCornerRadius(float radius) { m_cornerRadius = radius; }

        // False (the default) means PointerDownEvent/Tab traversal
        // never focus this ScrollView -- same meaning as Container's
        // own setFocusable() (container.hpp). See this class's own v1
        // note on why focus matters for wheel scrolling specifically.
        void setFocusable(bool focusable) { m_focusable = focusable; }

        // Pixels scrolled per unit of PointerWheelEvent::scrollY (or
        // scrollX, on a horizontal-only ScrollView -- see
        // handleEvent()'s own comment for exactly which axis a given
        // wheel event maps to).
        void setWheelScrollSpeed(float pixelsPerUnit) { m_wheelScrollSpeed = pixelsPerUnit; }

        // Pixels moved per Up/Down/Left/Right KeyDownEvent while this
        // ScrollView is focused; PageUp/PageDown move a full viewport
        // extent, Home/End jump to the matching scroll extreme on
        // whichever axis is scrollable (vertical preferred if both
        // are).
        void setKeyScrollSpeed(float pixelsPerKeypress) { m_keyScrollSpeed = pixelsPerKeypress; }

        // Thin, fixed-width overlay bar(s) drawn along the viewport's
        // own right/bottom edge, sized proportionally to
        // viewport/content extent and positioned by the current scroll
        // offset -- a visual indicator only, not an interactive drag
        // target (dsbox_ui has no pointer-capture/drag-gesture
        // machinery yet -- see Container::setOnClick()'s own note on
        // the same gap). Scroll by wheel or keyboard instead (see
        // setWheelScrollSpeed()/setKeyScrollSpeed()). Only drawn for
        // whichever axis is both scrollable and currently
        // overflowing (content bigger than viewport on that axis).
        void setShowScrollbars(bool show) { m_showScrollbars = show; }
        void setScrollbarColor(UiColor color) { m_scrollbarColor = color; }
        void setScrollbarThickness(float thickness) { m_scrollbarThickness = thickness; }

        // -- Scroll position --

        // Clamped immediately into [0, maxScrollX()]/[0, maxScrollY()]
        // as they currently stand -- which, since those depend on the
        // child's own measured size, are only meaningful AFTER at
        // least one measure()+arrange() pass; calling these before the
        // first tick() clamps against {0,0} (no overflow yet known),
        // same as every other element's "nothing drawn until the first
        // arrange()" starting state.
        void setScrollX(float x);
        void setScrollY(float y);
        void scrollBy(float dx, float dy);

        float scrollX() const noexcept { return m_scrollX; }
        float scrollY() const noexcept { return m_scrollY; }

        // Largest valid setScrollX()/setScrollY() value -- 0 if the
        // child is currently no larger than the viewport on that axis,
        // or if that axis isn't scrollable at all.
        float maxScrollX() const noexcept { return m_maxScrollX; }
        float maxScrollY() const noexcept { return m_maxScrollY; }

    private:
        void clampScroll() noexcept;

        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        // Holds at most one entry -- a std::vector (rather than a bare
        // std::unique_ptr<IUiElement>) purely so children() can return
        // a std::span over it directly, the same reason Container
        // holds its own children this way (container.hpp).
        std::vector<std::unique_ptr<IUiElement>> m_children;

        bool m_verticalScroll = true;
        bool m_horizontalScroll = false;
        bool m_focusable = false;

        UiLength m_preferredWidth = UiLength::pixels(280.0f);
        UiLength m_preferredHeight = UiLength::pixels(200.0f);

        UiColor m_backgroundColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        float m_cornerRadius = 0.0f;

        float m_wheelScrollSpeed = 40.0f;
        float m_keyScrollSpeed = 24.0f;

        bool m_showScrollbars = true;
        UiColor m_scrollbarColor{ 0.0f, 0.0f, 0.0f, 0.35f };
        float m_scrollbarThickness = 6.0f;

        float m_scrollX = 0.0f;
        float m_scrollY = 0.0f;
        float m_maxScrollX = 0.0f;
        float m_maxScrollY = 0.0f;

        UiSize m_childMeasuredSize{};
        UiRect m_bounds;
    };
}
