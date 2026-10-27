#include <dsbox_ui/scroll_view.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <algorithm>
#include <limits>
#include <variant>

namespace dsbox_ui
{
    ScrollView::ScrollView(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // every other element's destructor in this module -- see
    // Container's own comment (container.cpp).
    ScrollView::~ScrollView() = default;

    IUiElement& ScrollView::setChild(std::unique_ptr<IUiElement> child)
    {
        m_children.clear();
        m_children.push_back(std::move(child));
        return *m_children.back();
    }

    std::span<std::unique_ptr<IUiElement>> ScrollView::children()
    {
        return { m_children.data(), m_children.size() };
    }

    void ScrollView::clampScroll() noexcept
    {
        m_scrollX = std::clamp(m_scrollX, 0.0f, m_maxScrollX);
        m_scrollY = std::clamp(m_scrollY, 0.0f, m_maxScrollY);
    }

    void ScrollView::setScrollX(float x)
    {
        m_scrollX = std::clamp(x, 0.0f, m_maxScrollX);
    }

    void ScrollView::setScrollY(float y)
    {
        m_scrollY = std::clamp(y, 0.0f, m_maxScrollY);
    }

    void ScrollView::scrollBy(float dx, float dy)
    {
        setScrollX(m_scrollX + dx);
        setScrollY(m_scrollY + dy);
    }

    UiSize ScrollView::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        const float width = resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth);
        const float height = resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight);

        if (!m_children.empty() && m_children.front())
        {
            // The scrollable axis/axes are measured UNBOUNDED -- the
            // child's own natural size on that axis is what decides
            // how far this ScrollView can scroll (see arrange()'s own
            // maxScrollX/Y computation). The non-scrollable axis is
            // instead bounded to exactly this viewport's own extent,
            // stretching the child to fill it -- see this class's own
            // v1 note (scroll_view.hpp) on why there is no third option
            // here.
            UiConstraints childConstraints;
            childConstraints.minWidth = 0.0f;
            childConstraints.maxWidth = m_horizontalScroll
                ? std::numeric_limits<float>::infinity() : width;
            childConstraints.minHeight = 0.0f;
            childConstraints.maxHeight = m_verticalScroll
                ? std::numeric_limits<float>::infinity() : height;

            m_childMeasuredSize = m_children.front()->measure(childConstraints, backend);
        }
        else
        {
            m_childMeasuredSize = UiSize{};
        }

        return { width, height };
    }

    void ScrollView::arrange(const UiRect& rect, IUiRenderBackend& backend)
    {
        m_bounds = rect;

        m_maxScrollX = m_horizontalScroll ? std::max(0.0f, m_childMeasuredSize.width - rect.width) : 0.0f;
        m_maxScrollY = m_verticalScroll ? std::max(0.0f, m_childMeasuredSize.height - rect.height) : 0.0f;
        clampScroll();

        if (m_children.empty() || !m_children.front())
            return;

        // Positioned at an ABSOLUTE location shifted by the current
        // scroll offset -- see this class's own class comment for why
        // that alone is enough to make scrolling work with no new
        // DisplayList primitive. Typically partly or wholly outside
        // `rect` itself; paint()'s own PushClip/PopClip around this
        // child's paint() output is what keeps that invisible.
        const UiRect childRect{
            rect.x - m_scrollX, rect.y - m_scrollY,
            m_childMeasuredSize.width, m_childMeasuredSize.height
        };

        m_children.front()->arrange(childRect, backend);
    }

    void ScrollView::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;
        if (!hasArea)
            return;

        if (m_backgroundColor.a > 0.0f)
            out.rect(m_bounds, m_backgroundColor, m_cornerRadius);

        out.pushClip(m_bounds);

        if (!m_children.empty() && m_children.front())
            m_children.front()->paint(out);

        // Overlay scrollbars, drawn last (so they sit on top of the
        // child's own content) but still inside the same clip -- see
        // setShowScrollbars()'s own doc for why these are a visual
        // indicator only, not an interactive drag target.
        if (m_showScrollbars)
        {
            if (m_verticalScroll && m_maxScrollY > 0.0f)
            {
                const float contentHeight = m_childMeasuredSize.height;
                const float trackHeight = m_bounds.height;
                const float thumbHeight = std::max(
                    16.0f, trackHeight * (trackHeight / std::max(trackHeight, contentHeight)));
                const float thumbY = m_bounds.y + (trackHeight - thumbHeight) * (m_scrollY / m_maxScrollY);

                out.rect(
                    UiRect{ m_bounds.right() - m_scrollbarThickness, thumbY, m_scrollbarThickness, thumbHeight },
                    m_scrollbarColor, m_scrollbarThickness * 0.5f);
            }

            if (m_horizontalScroll && m_maxScrollX > 0.0f)
            {
                const float contentWidth = m_childMeasuredSize.width;
                const float trackWidth = m_bounds.width;
                const float thumbWidth = std::max(
                    16.0f, trackWidth * (trackWidth / std::max(trackWidth, contentWidth)));
                const float thumbX = m_bounds.x + (trackWidth - thumbWidth) * (m_scrollX / m_maxScrollX);

                out.rect(
                    UiRect{ thumbX, m_bounds.bottom() - m_scrollbarThickness, thumbWidth, m_scrollbarThickness },
                    m_scrollbarColor, m_scrollbarThickness * 0.5f);
            }
        }

        out.popClip();
    }

    bool ScrollView::handleEvent(const UiEvent& event, IUiElementActions& actions)
    {
        // Same "only react when THIS element is exactly what the
        // interaction is about" guard every other interactive element
        // in this module uses.
        if (event.phase != UiEventPhase::Target || event.target != m_id)
            return false;

        if (std::get_if<PointerDownEvent>(&event.payload))
        {
            if (m_focusable)
                actions.requestFocus(m_id); // see this class's own v1 note: focus is what makes wheel scrolling work afterward
            return true;
        }

        if (std::get_if<PointerUpEvent>(&event.payload))
            return true;

        if (const auto* wheel = std::get_if<PointerWheelEvent>(&event.payload))
        {
            // Vertical wheel motion scrolls the vertical axis if it's
            // scrollable, otherwise falls back to the horizontal axis
            // (a common trackpad/shift-wheel convenience); explicit
            // horizontal wheel motion (scrollX) always maps to the
            // horizontal axis when that's scrollable. Either way, a
            // ScrollView with NEITHER axis scrollable consumes nothing.
            bool handled = false;

            if (m_verticalScroll && wheel->scrollY != 0.0f)
            {
                setScrollY(m_scrollY - wheel->scrollY * m_wheelScrollSpeed);
                handled = true;
            }
            else if (m_horizontalScroll && wheel->scrollY != 0.0f)
            {
                setScrollX(m_scrollX - wheel->scrollY * m_wheelScrollSpeed);
                handled = true;
            }

            if (m_horizontalScroll && wheel->scrollX != 0.0f)
            {
                setScrollX(m_scrollX - wheel->scrollX * m_wheelScrollSpeed);
                handled = true;
            }

            if (handled)
                actions.markDirty(m_id);

            return handled;
        }

        if (const auto* keyDown = std::get_if<KeyDownEvent>(&event.payload))
        {
            switch (keyDown->key)
            {
                case dsbox::KeyCode::Up:
                    if (m_verticalScroll) { setScrollY(m_scrollY - m_keyScrollSpeed); actions.markDirty(m_id); }
                    return m_verticalScroll;

                case dsbox::KeyCode::Down:
                    if (m_verticalScroll) { setScrollY(m_scrollY + m_keyScrollSpeed); actions.markDirty(m_id); }
                    return m_verticalScroll;

                case dsbox::KeyCode::Left:
                    if (m_horizontalScroll) { setScrollX(m_scrollX - m_keyScrollSpeed); actions.markDirty(m_id); }
                    return m_horizontalScroll;

                case dsbox::KeyCode::Right:
                    if (m_horizontalScroll) { setScrollX(m_scrollX + m_keyScrollSpeed); actions.markDirty(m_id); }
                    return m_horizontalScroll;

                case dsbox::KeyCode::PageUp:
                    if (m_verticalScroll) { setScrollY(m_scrollY - m_bounds.height); actions.markDirty(m_id); return true; }
                    if (m_horizontalScroll) { setScrollX(m_scrollX - m_bounds.width); actions.markDirty(m_id); return true; }
                    return false;

                case dsbox::KeyCode::PageDown:
                    if (m_verticalScroll) { setScrollY(m_scrollY + m_bounds.height); actions.markDirty(m_id); return true; }
                    if (m_horizontalScroll) { setScrollX(m_scrollX + m_bounds.width); actions.markDirty(m_id); return true; }
                    return false;

                case dsbox::KeyCode::Home:
                    if (m_verticalScroll) { setScrollY(0.0f); actions.markDirty(m_id); return true; }
                    if (m_horizontalScroll) { setScrollX(0.0f); actions.markDirty(m_id); return true; }
                    return false;

                case dsbox::KeyCode::End:
                    if (m_verticalScroll) { setScrollY(m_maxScrollY); actions.markDirty(m_id); return true; }
                    if (m_horizontalScroll) { setScrollX(m_maxScrollX); actions.markDirty(m_id); return true; }
                    return false;

                default:
                    return false;
            }
        }

        return false;
    }
}
