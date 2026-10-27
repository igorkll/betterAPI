#include <dsbox_ui/dropdown.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <algorithm>
#include <variant>

namespace dsbox_ui
{
    Dropdown::Dropdown(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // every other element's destructor in this module -- see
    // Container's own comment (container.cpp).
    Dropdown::~Dropdown() = default;

    void Dropdown::setItems(std::vector<std::string> items)
    {
        m_items = std::move(items);
        m_needsReshape = true;

        if (m_items.empty())
        {
            m_selectedIndex = kInvalidIndex;
        }
        else if (m_selectedIndex != kInvalidIndex && m_selectedIndex >= m_items.size())
        {
            m_selectedIndex = m_items.size() - 1;
        }

        m_hoveredIndex = kInvalidIndex;
    }

    void Dropdown::setSelectedIndex(std::size_t index)
    {
        if (index != kInvalidIndex && (m_items.empty() || index >= m_items.size()))
            index = kInvalidIndex;

        m_selectedIndex = index;
        m_needsReshape = true; // header label (ComboBox mode) tracks the selected item's own text
    }

    void Dropdown::setOpen(bool open)
    {
        if (m_mode != DropdownMode::ComboBox)
            return; // always open in ListBox mode -- see DropdownMode's own doc

        if (open == m_open)
            return;

        m_open = open;
        m_hoveredIndex = kInvalidIndex;

        if (m_onOpenChanged)
            m_onOpenChanged(m_open);
    }

    void Dropdown::reshape(IUiRenderBackend& backend)
    {
        m_needsReshape = false;
        m_itemGlyphs.clear();
        m_selectedLabelGlyphs.clear();
        m_placeholderGlyphs.clear();

        if (m_font == kInvalidFontHandle)
        {
            m_metrics = TextMetrics{};
            return;
        }

        m_metrics = backend.measureText(m_font, std::string_view(" "));

        m_itemGlyphs.reserve(m_items.size());
        for (const std::string& item : m_items)
            m_itemGlyphs.push_back(item.empty() ? std::vector<ShapedGlyph>{} : backend.shapeText(m_font, item));

        if (m_mode == DropdownMode::ComboBox)
        {
            if (m_selectedIndex != kInvalidIndex && m_selectedIndex < m_itemGlyphs.size())
                m_selectedLabelGlyphs = m_itemGlyphs[m_selectedIndex];
            else if (!m_placeholder.empty())
                m_placeholderGlyphs = backend.shapeText(m_font, m_placeholder);
        }
    }

    float Dropdown::listHeight() const noexcept
    {
        const bool showList = (m_mode == DropdownMode::ListBox) || m_open;
        if (!showList)
            return 0.0f;

        const std::size_t visible = std::min(m_items.size(), m_maxVisibleItems);
        return static_cast<float>(visible) * m_itemHeight;
    }

    UiSize Dropdown::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        if (m_needsReshape)
            reshape(backend);

        const float width = resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth);
        const float natural = headerHeightResolved() + listHeight();

        return { width, constraints.clampHeight(natural) };
    }

    void Dropdown::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    namespace
    {
        void emitRowGlyphRun(
            DisplayListBuilder& out, const std::vector<ShapedGlyph>& glyphs,
            FontHandle font, float x, float baselineY, UiColor color)
        {
            if (font == kInvalidFontHandle || glyphs.empty())
                return;

            GlyphRunCommand run;
            run.font = font;
            run.x = x;
            run.y = baselineY;
            run.color = color;
            run.glyphIds.reserve(glyphs.size());
            run.glyphOffsets.reserve(glyphs.size());

            for (const ShapedGlyph& glyph : glyphs)
            {
                run.glyphIds.push_back(glyph.glyphId);
                run.glyphOffsets.push_back(glyph.xAdvance);
            }

            out.glyphRun(std::move(run));
        }
    }

    void Dropdown::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;
        if (!hasArea)
            return;

        const float lineHeight = std::max(1.0f, m_metrics.lineHeight);
        const float hHeight = headerHeightResolved();

        if (m_mode == DropdownMode::ComboBox)
        {
            const UiColor& border = m_focused ? m_focusedBorderColor : m_borderColor;
            const UiRect headerRect{ m_bounds.x, m_bounds.y, m_bounds.width, hHeight };

            out.rect(headerRect, border, m_cornerRadius);

            constexpr float kBorderWidth = 1.5f;
            const UiRect innerHeader{
                headerRect.x + kBorderWidth, headerRect.y + kBorderWidth,
                std::max(0.0f, headerRect.width - 2.0f * kBorderWidth),
                std::max(0.0f, headerRect.height - 2.0f * kBorderWidth)
            };

            if (innerHeader.width > 0.0f && innerHeader.height > 0.0f)
                out.rect(innerHeader, m_headerBackgroundColor, std::max(0.0f, m_cornerRadius - kBorderWidth));

            out.pushClip(headerRect);

            const float baselineY = headerRect.y + (hHeight - lineHeight) * 0.5f + m_metrics.ascent;
            const float textX = headerRect.x + kInset;

            if (m_selectedIndex != kInvalidIndex)
                emitRowGlyphRun(out, m_selectedLabelGlyphs, m_font, textX, baselineY, m_textColor);
            else
                emitRowGlyphRun(out, m_placeholderGlyphs, m_font, textX, baselineY, m_placeholderColor);

            out.popClip();
        }

        const bool showList = (m_mode == DropdownMode::ListBox) || m_open;
        if (!showList || m_items.empty())
            return;

        const std::size_t visibleCount = std::min(m_items.size(), m_maxVisibleItems);
        const UiRect listRect{ m_bounds.x, m_bounds.y + hHeight, m_bounds.width, static_cast<float>(visibleCount) * m_itemHeight };

        if (listRect.width <= 0.0f || listRect.height <= 0.0f)
            return;

        out.rect(listRect, m_itemBackgroundColor, 0.0f);

        if (m_mode == DropdownMode::ListBox)
            out.rect(listRect, m_borderColor, m_cornerRadius); // ListBox has no header of its own to carry the border -- draw it around the list instead

        out.pushClip(listRect);

        for (std::size_t i = 0; i < visibleCount; ++i)
        {
            const UiRect rowRect{ listRect.x, listRect.y + static_cast<float>(i) * m_itemHeight, listRect.width, m_itemHeight };

            if (i == m_selectedIndex && m_selectedItemColor.a > 0.0f)
                out.rect(rowRect, m_selectedItemColor, 0.0f);
            else if (i == m_hoveredIndex && m_hoveredItemColor.a > 0.0f)
                out.rect(rowRect, m_hoveredItemColor, 0.0f);

            const float baselineY = rowRect.y + (m_itemHeight - lineHeight) * 0.5f + m_metrics.ascent;
            if (i < m_itemGlyphs.size())
                emitRowGlyphRun(out, m_itemGlyphs[i], m_font, rowRect.x + kInset, baselineY, m_textColor);
        }

        out.popClip();
    }

    bool Dropdown::handleEvent(const UiEvent& event, IUiElementActions& actions)
    {
        if (event.phase != UiEventPhase::Target || event.target != m_id)
            return false;

        const float hHeight = headerHeightResolved();

        if (const auto* down = std::get_if<PointerDownEvent>(&event.payload))
        {
            if (!m_enabled)
                return false;

            actions.requestFocus(m_id);

            if (m_mode == DropdownMode::ComboBox && down->y < m_bounds.y + hHeight)
            {
                setOpen(!m_open);
                actions.markDirty(m_id);
                return true;
            }

            const bool showList = (m_mode == DropdownMode::ListBox) || m_open;
            if (showList && !m_items.empty())
            {
                const float localY = down->y - m_bounds.y - hHeight;
                if (localY >= 0.0f)
                {
                    const std::size_t visibleCount = std::min(m_items.size(), m_maxVisibleItems);
                    const std::size_t rowIndex = static_cast<std::size_t>(localY / m_itemHeight);

                    if (rowIndex < visibleCount)
                    {
                        if (rowIndex != m_selectedIndex)
                        {
                            m_selectedIndex = rowIndex;
                            m_needsReshape = true;
                            if (m_onSelectionChanged)
                                m_onSelectionChanged(m_selectedIndex);
                        }

                        setOpen(false); // no-op in ListBox mode -- see setOpen()'s own doc
                        actions.markDirty(m_id);
                        return true;
                    }
                }
            }

            return true;
        }

        if (std::get_if<PointerUpEvent>(&event.payload))
            return true;

        if (const auto* move = std::get_if<PointerMoveEvent>(&event.payload))
        {
            const bool showList = (m_mode == DropdownMode::ListBox) || m_open;
            if (!showList || m_items.empty())
                return false;

            const float localY = move->y - m_bounds.y - hHeight;
            std::size_t newHovered = kInvalidIndex;

            if (localY >= 0.0f)
            {
                const std::size_t visibleCount = std::min(m_items.size(), m_maxVisibleItems);
                const std::size_t rowIndex = static_cast<std::size_t>(localY / m_itemHeight);
                if (rowIndex < visibleCount)
                    newHovered = rowIndex;
            }

            if (newHovered != m_hoveredIndex)
            {
                m_hoveredIndex = newHovered;
                actions.markDirty(m_id);
            }

            return false; // observational only -- never consumes the move
        }

        if (std::holds_alternative<FocusInEvent>(event.payload))
        {
            m_focused = true;
            actions.markDirty(m_id);
            return true;
        }

        if (std::holds_alternative<FocusOutEvent>(event.payload))
        {
            m_focused = false;
            m_hoveredIndex = kInvalidIndex;
            if (m_mode == DropdownMode::ComboBox && m_open)
                setOpen(false); // auto-collapse on blur, same as a native combobox
            actions.markDirty(m_id);
            return true;
        }

        if (const auto* keyDown = std::get_if<KeyDownEvent>(&event.payload))
        {
            if (!m_enabled)
                return false;

            if (keyDown->key == dsbox::KeyCode::Escape && m_mode == DropdownMode::ComboBox && m_open)
            {
                setOpen(false);
                actions.markDirty(m_id);
                return true;
            }

            if ((keyDown->key == dsbox::KeyCode::Enter || keyDown->key == dsbox::KeyCode::KeypadEnter
                 || keyDown->key == dsbox::KeyCode::Space)
                && !keyDown->repeat && m_mode == DropdownMode::ComboBox)
            {
                setOpen(!m_open);
                actions.markDirty(m_id);
                return true;
            }

            if (keyDown->key == dsbox::KeyCode::Up || keyDown->key == dsbox::KeyCode::Down)
            {
                if (m_items.empty())
                    return true;

                std::size_t next = m_selectedIndex;
                if (keyDown->key == dsbox::KeyCode::Up)
                    next = (m_selectedIndex == kInvalidIndex || m_selectedIndex == 0) ? 0 : m_selectedIndex - 1;
                else
                    next = (m_selectedIndex == kInvalidIndex) ? 0 : std::min(m_selectedIndex + 1, m_items.size() - 1);

                if (next != m_selectedIndex)
                {
                    m_selectedIndex = next;
                    m_needsReshape = true;
                    actions.markDirty(m_id);
                    if (m_onSelectionChanged)
                        m_onSelectionChanged(m_selectedIndex);
                }

                return true;
            }

            return false;
        }

        return false;
    }
}
