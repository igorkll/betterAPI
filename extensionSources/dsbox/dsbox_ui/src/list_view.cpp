#include <dsbox_ui/list_view.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <algorithm>
#include <cmath>
#include <variant>

namespace dsbox_ui
{
    ListView::ListView(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // every other element's destructor in this module -- see
    // Container's own comment (container.cpp).
    ListView::~ListView() = default;

    void ListView::setRows(std::vector<std::vector<std::string>> rows)
    {
        m_rows = std::move(rows);
        m_needsReshape = true;

        if (m_rows.empty())
        {
            m_selectedIndex = kInvalidIndex;
        }
        else if (m_selectedIndex != kInvalidIndex && m_selectedIndex >= m_rows.size())
        {
            m_selectedIndex = m_rows.size() - 1;
        }
    }

    void ListView::setSelectedIndex(std::size_t index)
    {
        if (index != kInvalidIndex && (m_rows.empty() || index >= m_rows.size()))
            index = kInvalidIndex;

        m_selectedIndex = index;
    }

    void ListView::reshape(IUiRenderBackend& backend)
    {
        m_needsReshape = false;
        m_headerGlyphs.clear();
        m_cellGlyphs.clear();

        if (m_font == kInvalidFontHandle)
        {
            m_metrics = TextMetrics{};
            return;
        }

        // Stable probe for ascent/lineHeight, same reasoning
        // TextField/TextArea's own measure() uses a bare space for --
        // every cell is set in the same font/size, so one shared
        // TextMetrics positions every row.
        m_metrics = backend.measureText(m_font, std::string_view(" "));

        m_headerGlyphs.reserve(m_columns.size());
        for (const ListViewColumn& column : m_columns)
            m_headerGlyphs.push_back(column.title.empty() ? std::vector<ShapedGlyph>{} : backend.shapeText(m_font, column.title));

        m_cellGlyphs.reserve(m_rows.size());
        for (const std::vector<std::string>& row : m_rows)
        {
            std::vector<std::vector<ShapedGlyph>> rowGlyphs;
            rowGlyphs.reserve(row.size());
            for (const std::string& cell : row)
                rowGlyphs.push_back(cell.empty() ? std::vector<ShapedGlyph>{} : backend.shapeText(m_font, cell));
            m_cellGlyphs.push_back(std::move(rowGlyphs));
        }
    }

    UiSize ListView::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        if (m_needsReshape)
            reshape(backend);

        const float width = resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth);

        float height;
        if (m_preferredHeight.unit == UiUnit::Content)
        {
            // fit-content: exactly enough for the header plus every
            // row, clamped into whatever the parent allows -- see this
            // class's own doc on setPreferredHeight() for why Content
            // is the default (the usual ListView-in-a-ScrollView case).
            const float natural = headerHeight() + static_cast<float>(m_rows.size()) * m_rowHeight;
            height = constraints.clampHeight(natural);
        }
        else
        {
            height = resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight);
        }

        return { width, height };
    }

    void ListView::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    namespace
    {
        constexpr float kCellInset = 6.0f; // mirrors ListView::kCellInset -- see this function's own call sites

        void emitCellGlyphRun(
            DisplayListBuilder& out, const UiRect& cellRect, const std::vector<ShapedGlyph>& glyphs,
            FontHandle font, float baselineY, UiColor color)
        {
            if (font == kInvalidFontHandle || glyphs.empty() || cellRect.width <= 0.0f)
                return;

            // Clip each cell's own text to its own column -- a value
            // wider than its column must never bleed into the next
            // one. Mirrors TextField/TextArea's own "clip the whole
            // field" pushClip usage (text_field.cpp/text_area.cpp),
            // scoped per cell instead of per element.
            out.pushClip(cellRect);

            GlyphRunCommand run;
            run.font = font;
            run.x = cellRect.x + kCellInset;
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
            out.popClip();
        }
    }

    void ListView::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;
        if (!hasArea)
            return;

        out.rect(m_bounds, m_borderColor, 0.0f);

        constexpr float kBorderWidth = 1.0f;
        const UiRect inner{
            m_bounds.x + kBorderWidth, m_bounds.y + kBorderWidth,
            std::max(0.0f, m_bounds.width - 2.0f * kBorderWidth),
            std::max(0.0f, m_bounds.height - 2.0f * kBorderWidth)
        };

        if (inner.width <= 0.0f || inner.height <= 0.0f)
            return;

        // Content taller than this element's own bounds is clipped at
        // its bottom edge -- same "no scrolling of its own" v1 note as
        // TextArea's own paint() (text_area.cpp). Wrap this element in
        // a ScrollView for one that scrolls.
        out.pushClip(inner);

        const float hHeight = headerHeight();
        const float lineHeight = std::max(1.0f, m_metrics.lineHeight);

        if (m_showHeader && !m_columns.empty())
        {
            const UiRect headerRect{ inner.x, inner.y, inner.width, hHeight };
            out.rect(headerRect, m_headerBackgroundColor, 0.0f);

            const float baselineY = headerRect.y + (hHeight - lineHeight) * 0.5f + m_metrics.ascent;

            float cursorX = inner.x;
            for (std::size_t c = 0; c < m_columns.size(); ++c)
            {
                const UiRect cellRect{ cursorX, headerRect.y, m_columns[c].width, hHeight };

                if (c < m_headerGlyphs.size())
                    emitCellGlyphRun(out, cellRect, m_headerGlyphs[c], m_font, baselineY, m_headerTextColor);

                cursorX += m_columns[c].width;
            }
        }

        float rowTop = inner.y + hHeight;

        for (std::size_t r = 0; r < m_rows.size(); ++r)
        {
            const UiRect rowRect{ inner.x, rowTop, inner.width, m_rowHeight };

            // Skip rows entirely below the visible/clipped area --
            // paint() still emits their commands (PushClipCommand
            // already restricts them to nothing visible), but skipping
            // outright keeps a very long row list cheap to paint, same
            // spirit as Container::paint()'s own "nothing meaningful to
            // draw" early-out for a degenerate rect.
            if (rowRect.y >= inner.bottom())
                break;

            UiColor rowColor = m_rowBackgroundColor;
            if (r % 2 == 1 && m_alternateRowColor.a > 0.0f)
                rowColor = m_alternateRowColor;
            if (r == m_selectedIndex && m_selectedRowColor.a > 0.0f)
                rowColor = m_selectedRowColor;

            if (rowColor.a > 0.0f)
                out.rect(rowRect, rowColor, 0.0f);

            const float baselineY = rowRect.y + (m_rowHeight - lineHeight) * 0.5f + m_metrics.ascent;

            float cursorX = inner.x;
            const std::vector<std::vector<ShapedGlyph>>* rowGlyphs = (r < m_cellGlyphs.size()) ? &m_cellGlyphs[r] : nullptr;

            for (std::size_t c = 0; c < m_columns.size(); ++c)
            {
                const UiRect cellRect{ cursorX, rowRect.y, m_columns[c].width, m_rowHeight };

                if (rowGlyphs && c < rowGlyphs->size())
                    emitCellGlyphRun(out, cellRect, (*rowGlyphs)[c], m_font, baselineY, m_textColor);

                cursorX += m_columns[c].width;

                if (m_gridLineColor.a > 0.0f && c + 1 < m_columns.size())
                    out.rect(UiRect{ cursorX, rowRect.y, 1.0f, m_rowHeight }, m_gridLineColor, 0.0f);
            }

            if (m_gridLineColor.a > 0.0f)
                out.rect(UiRect{ rowRect.x, rowRect.bottom() - 1.0f, rowRect.width, 1.0f }, m_gridLineColor, 0.0f);

            rowTop += m_rowHeight;
        }

        out.popClip();
    }

    bool ListView::handleEvent(const UiEvent& event, IUiElementActions& actions)
    {
        if (event.phase != UiEventPhase::Target || event.target != m_id)
            return false;

        if (const auto* down = std::get_if<PointerDownEvent>(&event.payload))
        {
            actions.requestFocus(m_id);

            if (!m_rows.empty())
            {
                const float localY = down->y - m_bounds.y - headerHeight();
                if (localY >= 0.0f)
                {
                    const std::size_t rowIndex = static_cast<std::size_t>(localY / m_rowHeight);
                    if (rowIndex < m_rows.size() && rowIndex != m_selectedIndex)
                    {
                        m_selectedIndex = rowIndex;
                        actions.markDirty(m_id);
                        if (m_onSelectionChanged)
                            m_onSelectionChanged(m_selectedIndex);
                    }
                }
            }

            return true;
        }

        if (std::get_if<PointerUpEvent>(&event.payload))
            return true;

        if (const auto* keyDown = std::get_if<KeyDownEvent>(&event.payload))
        {
            if (!m_enabled || m_rows.empty())
                return false;

            if (keyDown->key == dsbox::KeyCode::Up)
            {
                const std::size_t next = (m_selectedIndex == kInvalidIndex || m_selectedIndex == 0)
                    ? 0 : m_selectedIndex - 1;
                if (next != m_selectedIndex)
                {
                    m_selectedIndex = next;
                    actions.markDirty(m_id);
                    if (m_onSelectionChanged)
                        m_onSelectionChanged(m_selectedIndex);
                }
                return true;
            }

            if (keyDown->key == dsbox::KeyCode::Down)
            {
                const std::size_t next = (m_selectedIndex == kInvalidIndex)
                    ? 0 : std::min(m_selectedIndex + 1, m_rows.size() - 1);
                if (next != m_selectedIndex)
                {
                    m_selectedIndex = next;
                    actions.markDirty(m_id);
                    if (m_onSelectionChanged)
                        m_onSelectionChanged(m_selectedIndex);
                }
                return true;
            }

            if ((keyDown->key == dsbox::KeyCode::Enter || keyDown->key == dsbox::KeyCode::KeypadEnter)
                && !keyDown->repeat && m_selectedIndex != kInvalidIndex && m_onRowActivated)
            {
                m_onRowActivated(m_selectedIndex);
                return true;
            }

            return false;
        }

        return false;
    }
}
