#include <dsbox_ui/container.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <algorithm>
#include <cmath>
#include <variant>

namespace dsbox_ui
{
    namespace
    {
        // UiConstraints has no operator== of its own (length.hpp) --
        // field-by-field float equality is safe here specifically
        // because these values are never independently computed or
        // perturbed between the two comparisons that use this: a
        // parent's ILayoutStrategy passes the exact same floats back
        // to measure() when nothing about the parent's own layout
        // changed (see Container::measure()'s own cache check).
        bool constraintsEqual(const UiConstraints& a, const UiConstraints& b) noexcept
        {
            return a.minWidth == b.minWidth && a.maxWidth == b.maxWidth
                && a.minHeight == b.minHeight && a.maxHeight == b.maxHeight;
        }
    }

    Container::Container(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // WindowUiTree's destructor -- see that class's own comment.
    Container::~Container() = default;

    void Container::setPadding(float top, float right, float bottom, float left)
    {
        m_paddingTop = top;
        m_paddingRight = right;
        m_paddingBottom = bottom;
        m_paddingLeft = left;
    }

    void Container::setLayoutStrategy(std::unique_ptr<ILayoutStrategy> strategy)
    {
        m_layoutStrategy = std::move(strategy);
    }

    IUiElement& Container::addChild(std::unique_ptr<IUiElement> child)
    {
        m_children.push_back(std::move(child));
        return *m_children.back();
    }

    std::span<std::unique_ptr<IUiElement>> Container::children()
    {
        return { m_children.data(), m_children.size() };
    }

    bool Container::prepareIncrementalRelayout(const std::unordered_set<UiElementId>& dirtyIds)
    {
        bool anyDirty = dirtyIds.find(m_id) != dirtyIds.end();

        for (auto& child : m_children)
        {
            if (!child)
                continue;

            // Deliberately NOT short-circuited once anyDirty is
            // already true -- every child's OWN cached state must be
            // refreshed on every call (a child dirty last time may be
            // clean now, and vice versa; see IUiElement::
            // prepareIncrementalRelayout()'s own doc, element.hpp).
            const bool childClean = child->prepareIncrementalRelayout(dirtyIds);
            if (!childClean)
                anyDirty = true;
        }

        m_relayoutDirty = anyDirty;
        return !anyDirty;
    }

    void Container::forceFullRelayout()
    {
        m_relayoutDirty = true;

        for (auto& child : m_children)
            if (child)
                child->forceFullRelayout();
    }

    float Container::flexGrow(UiAxis axis) const noexcept
    {
        const UiLength& length = (axis == UiAxis::Horizontal) ? m_preferredWidth : m_preferredHeight;
        return length.unit == UiUnit::Fraction ? std::max(0.0f, length.value) : 0.0f;
    }

    UiSize Container::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        // Incremental relayout cache check -- see
        // prepareIncrementalRelayout()'s own doc (container.hpp) and
        // IUiElement's (element.hpp) for the full contract. Safe
        // specifically because m_relayoutDirty is only ever false when
        // a RECENT prepareIncrementalRelayout() call proved neither
        // this Container nor anything in its subtree was markDirty()'d
        // at UiDirtyLevel::Layout since the last pass -- the incoming
        // `constraints` comparison on top of that guards against the
        // (rarer) case where this Container is clean but its PARENT'S
        // own layout still happens to hand it different constraints
        // than last time for some other reason.
        if (!m_relayoutDirty && constraintsEqual(constraints, m_lastMeasureConstraints))
            return m_lastMeasureSize;

        const bool widthIsContent = m_preferredWidth.unit == UiUnit::Content;
        const bool heightIsContent = m_preferredHeight.unit == UiUnit::Content;

        // UiUnit::Content (length.hpp -- fit-content) is the one unit
        // resolveLengthDimension() genuinely cannot answer on its own
        // (it falls back to filling the incoming max bound for Content
        // same as it does for Auto/Fraction -- see its own doc). This
        // is the real two-pass measure Container's own v1 sizing
        // limitation comment used to call out as missing: ask the
        // layout strategy what its children actually need, via
        // ILayoutStrategy::measureContent() (element.hpp), then resolve
        // THIS Container's own box from that instead of from
        // m_preferredWidth/Height directly. A Container with no
        // strategy, or no children, has nothing to measure content
        // from and falls through to the old behavior below -- same
        // result resolveLengthDimension() already gave Content before
        // this existed (fills the incoming max bound).
        if ((widthIsContent || heightIsContent) && m_layoutStrategy && !m_children.empty())
        {
            UiConstraints contentConstraints;
            contentConstraints.minWidth = 0.0f;
            contentConstraints.minHeight = 0.0f;
            contentConstraints.maxWidth = std::isfinite(constraints.maxWidth)
                ? std::max(0.0f, constraints.maxWidth - m_paddingLeft - m_paddingRight)
                : constraints.maxWidth;
            contentConstraints.maxHeight = std::isfinite(constraints.maxHeight)
                ? std::max(0.0f, constraints.maxHeight - m_paddingTop - m_paddingBottom)
                : constraints.maxHeight;

            const UiSize contentSize = m_layoutStrategy->measureContent(children(), contentConstraints, backend);

            const float resolvedWidth = widthIsContent
                ? constraints.clampWidth(contentSize.width + m_paddingLeft + m_paddingRight)
                : resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth);

            const float resolvedHeight = heightIsContent
                ? constraints.clampHeight(contentSize.height + m_paddingTop + m_paddingBottom)
                : resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight);

            m_lastMeasureConstraints = constraints;
            m_lastMeasureSize = { resolvedWidth, resolvedHeight };
            return m_lastMeasureSize;
        }

        m_lastMeasureConstraints = constraints;
        m_lastMeasureSize = {
            resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth),
            resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight)
        };
        return m_lastMeasureSize;
    }

    void Container::arrange(const UiRect& rect, IUiRenderBackend& backend)
    {
        // `rect` is compared against the PREVIOUS m_bounds before it's
        // overwritten below -- see this method's own skip check just
        // after. Position can shift even for a Container whose own
        // subtree is entirely clean (an earlier sibling elsewhere in
        // the SAME parent grew/shrank, moving everything after it), so
        // skipping is only safe when BOTH this subtree is clean AND
        // this Container's own absolute position/size didn't change
        // either -- clean-but-moved still needs a real arrange() pass
        // (which will itself skip recursing into clean children whose
        // OWN positions, relative to this content rect, are unaffected
        // by a move of the whole Container).
        const bool rectUnchanged = (rect == m_bounds);
        m_bounds = rect;

        if (!m_relayoutDirty && rectUnchanged)
            return; // this subtree is both unchanged and hasn't moved -- children already hold correct bounds from a previous pass; see prepareIncrementalRelayout()'s own doc (container.hpp) for the full mechanism

        if (!m_layoutStrategy)
            return;

        const UiRect contentRect{
            rect.x + m_paddingLeft,
            rect.y + m_paddingTop,
            std::max(0.0f, rect.width - m_paddingLeft - m_paddingRight),
            std::max(0.0f, rect.height - m_paddingTop - m_paddingBottom)
        };

        m_layoutStrategy->layout(children(), contentRect, backend);
    }

    void Container::paint(DisplayListBuilder& out) const
    {
        // A degenerate container (zero or negative area -- e.g. a
        // StackLayout child that got clamped down to nothing because
        // there was no room left, see StackLayout's own v1 note) has
        // nothing meaningful to draw. Skipping it here means the
        // renderer never even sees a command for it, rather than
        // relying on the GPU scissor/rasterizer to silently discard a
        // zero-size primitive.
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;

        // isPressed() (see setPressedColor()'s own doc) overrides the
        // regular background color for as long as it's true and an
        // override color was actually configured -- otherwise paint()
        // behaves exactly as it did before setPressedColor() existed.
        const UiColor& baseFillColor =
            (m_pressed && m_pressedColor.a > 0.0f) ? m_pressedColor :
            (m_hovered && m_hoverColor.a > 0.0f) ? m_hoverColor :
            m_backgroundColor;

        // setOpacity()'s own doc covers exactly what this does and
        // does not affect -- m_opacity == 1.0f (the default) leaves
        // fillColor.a identical to baseFillColor.a, so nothing here
        // changes behavior for a Container that never calls
        // setOpacity().
        UiColor fillColor = baseFillColor;
        fillColor.a *= m_opacity;

        if (hasArea && fillColor.a > 0.0f)
            out.rect(m_bounds, fillColor, m_cornerRadius);

        if (!hasArea)
            return; // children are positioned relative to a container with no area -- nothing they draw could be meaningfully visible either

        if (m_clipsChildren)
            out.pushClip(m_bounds);

        for (const auto& child : m_children)
            if (child)
                child->paint(out);

        if (m_clipsChildren)
            out.popClip();
    }

    bool Container::handleEvent(const UiEvent& event, IUiElementActions& actions)
    {
        // React whenever this element is either the actual hit
        // target (Target phase) OR an ancestor a non-consuming
        // descendant's event is bubbling back through (Bubble phase)
        // -- see this method's own declaration in container.hpp for
        // the full reasoning. Capture is still skipped entirely: a
        // Container must not react to a press/release that is only
        // passing through it on the way DOWN to whatever the real hit
        // element turns out to be -- only on the way back up once that
        // element has had (and declined) its own chance to handle it.
        //
        // This is what makes e.g. makeButton()'s label Text actually
        // clickable: Text::handleEvent() always returns false (see
        // text.hpp -- it is non-interactive), so a PointerDownEvent/
        // PointerUpEvent hit-tested onto that label reaches this
        // Container during Bubble with event.target still equal to
        // the label's own id, never this Container's. Gating on
        // event.target == m_id as well as phase == Target (the old
        // behavior) silently dropped every one of those events --
        // clicking the label did nothing, only clicking the bare
        // background around it worked.
        if (event.phase == UiEventPhase::Capture)
            return false;

        // A plain, unconfigured Container (no onClick, not focusable --
        // a pure layout wrapper, the equivalent of a <div> with
        // neither onclick nor tabindex) must stay transparent to a
        // click BUBBLING UP from one of its own children: only Target
        // phase (this element was itself directly hit) keeps tracking
        // m_pressed/isPressed() unconditionally below, matching this
        // class's pre-existing behavior for every Container regardless
        // of interactivity configuration. Without this guard, EVERY
        // Container ancestor along a bubble path -- not just the
        // nearest one actually configured to care -- would swallow the
        // event purely by virtue of being a Container, which is a much
        // bigger behavior change than "a button's own label is
        // clickable" and would break something as ordinary as a
        // decorative wrapper Container sitting between a button and
        // the rest of the tree.
        //
        // PointerEnterEvent/PointerLeaveEvent are unaffected -- see
        // this method's own handling of them further down -- those are
        // always dispatched directly at Target phase to each element
        // on the hovered path (WindowUiTree::updateHoverState()), never
        // bubbled, so isBubble is never true for them regardless of
        // this Container's own interactivity.
        const bool isBubble = event.phase == UiEventPhase::Bubble;
        const bool interactive = m_focusable || static_cast<bool>(m_onClick);

        if (isBubble && !interactive)
            return false;

        if (std::get_if<PointerDownEvent>(&event.payload))
        {
            m_pressed = true;
            actions.markDirty(m_id, UiDirtyLevel::Repaint); // pressed-color change, if any -- paint() needs to run again, layout never does

            if (m_focusable)
                actions.requestFocus(m_id);

            // Capture the pointer for the duration of this press: a
            // drag that leaves this element's bounds -- or the window
            // altogether -- before the button is released must still
            // deliver that PointerUpEvent here, or m_pressed would stay
            // true forever (this element would never see the up event
            // that's supposed to clear it; see
            // IUiElementActions::setPointerCapture()'s own doc).
            actions.setPointerCapture(m_id);

            return true;
        }

        if (std::get_if<PointerUpEvent>(&event.payload))
        {
            const bool wasPressed = m_pressed;
            m_pressed = false;
            actions.markDirty(m_id, UiDirtyLevel::Repaint);

            // Unchanged from before pointer capture existed: any
            // PointerUpEvent delivered to this element after a matching
            // PointerDownEvent fires the click, regardless of where the
            // release happened to land -- capture (see
            // PointerDownEvent's own handler above) only changes
            // whether this element gets to see that PointerUpEvent at
            // all; it never gates what happens once it does.
            if (wasPressed && m_onClick)
                m_onClick();

            // Capture auto-releases once this element has actually
            // received the PointerUpEvent (see WindowUiTree::
            // dispatchEvent()'s own comment) -- no explicit
            // releasePointerCapture() call needed here.

            return true;
        }

        // Keyboard activation (Enter/Space) for a focused, clickable
        // Container -- the keyboard equivalent of a pointer click on a
        // focusable HTML element (a <button>, or a <div tabindex="0">
        // with a key handler). Only fires on the initial press, not
        // OS autorepeat -- holding Enter down does not fire m_onClick()
        // over and over.
        if (const auto* keyDown = std::get_if<KeyDownEvent>(&event.payload))
        {
            if (m_onClick && !keyDown->repeat &&
                (keyDown->key == dsbox::KeyCode::Enter || keyDown->key == dsbox::KeyCode::Space))
            {
                m_onClick();
                return true;
            }

            return false;
        }

        if (std::get_if<PointerEnterEvent>(&event.payload))
        {
            m_hovered = true;
            actions.markDirty(m_id, UiDirtyLevel::Repaint); // hover-color change, if any -- see PointerDownEvent's own markDirty() call just above for why this is Repaint, not Layout
            return true;
        }

        if (std::get_if<PointerLeaveEvent>(&event.payload))
        {
            m_hovered = false;
            actions.markDirty(m_id, UiDirtyLevel::Repaint);
            return true;
        }

        // A gesture that was mid-press on THIS element ended some other
        // way than a matching PointerUpEvent (focus moved away, or
        // whatever started the press explicitly cancelled it -- see
        // PointerCancelEvent's own doc, event.hpp). Treated exactly
        // like "the press didn't complete": clear m_pressed WITHOUT
        // calling m_onClick(), same as a PointerUpEvent that arrives
        // with wasPressed already false. Can only ever arrive here at
        // Target phase, directly from WindowUiTree::cancelPointerCapture()
        // -- like Enter/Leave, this is never hit-tested or bubbled, so
        // the isBubble/interactive guard above is a no-op for it either
        // way.
        if (std::get_if<PointerCancelEvent>(&event.payload))
        {
            m_pressed = false;
            actions.markDirty(m_id, UiDirtyLevel::Repaint);
            return true;
        }

        return false;
    }
}
