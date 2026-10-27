#include <dsbox_ui/text_field.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <algorithm>
#include <cmath>

namespace dsbox_ui
{
    TextField::TextField(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // Container's destructor -- see that class's own comment.
    TextField::~TextField() = default;

    void TextField::setText(std::string text)
    {
        m_text = std::move(text);
        m_cursor = m_text.size();
    }

    float TextField::glyphOffsetForByte(std::size_t byteOffset) const noexcept
    {
        if (m_text.empty())
            return 0.0f; // see this method's own doc -- m_metrics may currently describe something else entirely

        const std::size_t index = std::min(byteOffset, m_shapedGlyphs.size());
        if (index < m_shapedGlyphs.size())
            return m_shapedGlyphs[index].xAdvance;
        return m_metrics.advanceWidth; // caret after the last glyph
    }

    std::size_t TextField::byteOffsetForLocalX(float localX) const noexcept
    {
        if (m_shapedGlyphs.empty())
            return 0;

        std::size_t best = 0;
        float bestDistance = std::abs(localX - glyphOffsetForByte(0));

        for (std::size_t i = 1; i <= m_shapedGlyphs.size(); ++i)
        {
            const float distance = std::abs(localX - glyphOffsetForByte(i));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }

        return best;
    }

    UiSize TextField::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        if (m_font == kInvalidFontHandle)
        {
            m_metrics = TextMetrics{};
            m_shapedGlyphs.clear();
            m_placeholderGlyphs.clear();
        }
        else if (!m_text.empty())
        {
            m_metrics = backend.measureText(m_font, m_text);
            m_shapedGlyphs = backend.shapeText(m_font, m_text);
            m_placeholderGlyphs.clear();
        }
        else
        {
            // No text yet -- still measure SOMETHING (the placeholder,
            // or a bare space) purely for lineHeight/ascent, so the
            // caret has a sensible height even before the first
            // keystroke; shape the placeholder too, if there is one,
            // so paint() can draw it without touching `backend` itself
            // (see Text's own "v1 shaping/caching model", text.hpp,
            // for why paint() never calls into the backend).
            m_shapedGlyphs.clear();

            const std::string_view measureSource = m_placeholder.empty() ? std::string_view(" ") : std::string_view(m_placeholder);
            m_metrics = backend.measureText(m_font, measureSource);
            m_placeholderGlyphs = m_placeholder.empty() ? std::vector<ShapedGlyph>{} : backend.shapeText(m_font, m_placeholder);
        }

        return {
            resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth),
            resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight)
        };
    }

    void TextField::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    UiRect TextField::textInputRect() const noexcept
    {
        const float caretX = m_bounds.x + kHorizontalInset + glyphOffsetForByte(m_cursor);
        const float caretY = m_bounds.y + (m_bounds.height - m_metrics.lineHeight) * 0.5f;
        return UiRect{ caretX, caretY, 2.0f, std::max(1.0f, m_metrics.lineHeight) };
    }

    void TextField::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;
        if (!hasArea)
            return;

        const UiColor& border = m_focused ? m_focusedBorderColor : m_borderColor;

        // A "border" is faked as two nested rounded rects -- an outer
        // one in `border`, an inner one (inset by kBorderWidth, in
        // m_backgroundColor) on top -- since DisplayList has no
        // stroke-only rect command (RectCommand is always filled, see
        // display_list.hpp). Cheap and good enough for a 1-2px field
        // outline; not a general-purpose border primitive.
        out.rect(m_bounds, border, m_cornerRadius);

        constexpr float kBorderWidth = 1.5f;
        const UiRect inner{
            m_bounds.x + kBorderWidth, m_bounds.y + kBorderWidth,
            std::max(0.0f, m_bounds.width - 2.0f * kBorderWidth),
            std::max(0.0f, m_bounds.height - 2.0f * kBorderWidth)
        };

        if (inner.width > 0.0f && inner.height > 0.0f)
            out.rect(inner, m_backgroundColor, std::max(0.0f, m_cornerRadius - kBorderWidth));

        // Text/caret must never paint outside this field's own box --
        // see this class's own "no horizontal scrolling" v1 note.
        out.pushClip(m_bounds);

        const float baselineY = m_bounds.y + (m_bounds.height - m_metrics.lineHeight) * 0.5f + m_metrics.ascent;
        const float textOriginX = m_bounds.x + kHorizontalInset;

        auto emitGlyphRun = [&](const std::vector<ShapedGlyph>& glyphs, UiColor color)
        {
            if (glyphs.empty())
                return;

            GlyphRunCommand run;
            run.font = m_font;
            run.x = textOriginX;
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
        };

        if (m_font != kInvalidFontHandle)
        {
            if (!m_text.empty())
                emitGlyphRun(m_shapedGlyphs, m_textColor);
            else
                emitGlyphRun(m_placeholderGlyphs, m_placeholderColor);
        }

        if (m_focused)
        {
            const float caretX = textOriginX + glyphOffsetForByte(m_cursor);
            const UiRect caret{
                caretX, m_bounds.y + (m_bounds.height - m_metrics.lineHeight) * 0.5f,
                1.5f, std::max(1.0f, m_metrics.lineHeight)
            };
            out.rect(caret, m_textColor);
        }

        out.popClip();
    }

    bool TextField::handleEvent(const UiEvent& event, IUiElementActions& actions)
    {
        // Same "only react when THIS element is exactly what the
        // interaction is about" guard Container::handleEvent() uses --
        // see that method's own comment (container.cpp).
        if (event.phase != UiEventPhase::Target || event.target != m_id)
            return false;

        if (const auto* down = std::get_if<PointerDownEvent>(&event.payload))
        {
            actions.requestFocus(m_id);
            m_cursor = m_text.empty() ? 0 : byteOffsetForLocalX(down->x - (m_bounds.x + kHorizontalInset));
            actions.markDirty(m_id);
            return true;
        }

        if (std::get_if<PointerUpEvent>(&event.payload))
            return true; // consumed so it doesn't fall through to whatever's behind this field; nothing else to do

        if (std::holds_alternative<FocusInEvent>(event.payload))
        {
            m_focused = true;
            actions.markDirty(m_id);
            return true;
        }

        if (std::holds_alternative<FocusOutEvent>(event.payload))
        {
            m_focused = false;
            actions.markDirty(m_id);
            return true;
        }

        if (const auto* textInput = std::get_if<TextInputEvent>(&event.payload))
        {
            if (!m_enabled || textInput->text.empty())
                return false;

            m_text.insert(m_cursor, textInput->text);
            m_cursor += textInput->text.size();
            actions.markDirty(m_id);

            if (m_onChange)
                m_onChange(m_text);

            return true;
        }

        if (std::holds_alternative<TextEditingEvent>(event.payload))
            return true; // accepted, not rendered -- see this class's own "v1 scope"

        if (const auto* keyDown = std::get_if<KeyDownEvent>(&event.payload))
        {
            if (!m_enabled)
                return false;

            switch (keyDown->key)
            {
                case dsbox::KeyCode::Backspace:
                    if (m_cursor > 0)
                    {
                        // One BYTE, not necessarily one character --
                        // see this class's own "v1 scope".
                        m_text.erase(m_cursor - 1, 1);
                        --m_cursor;
                        actions.markDirty(m_id);
                        if (m_onChange)
                            m_onChange(m_text);
                    }
                    return true;

                case dsbox::KeyCode::Delete:
                    if (m_cursor < m_text.size())
                    {
                        m_text.erase(m_cursor, 1);
                        actions.markDirty(m_id);
                        if (m_onChange)
                            m_onChange(m_text);
                    }
                    return true;

                case dsbox::KeyCode::Left:
                    if (m_cursor > 0)
                    {
                        --m_cursor;
                        actions.markDirty(m_id);
                    }
                    return true;

                case dsbox::KeyCode::Right:
                    if (m_cursor < m_text.size())
                    {
                        ++m_cursor;
                        actions.markDirty(m_id);
                    }
                    return true;

                case dsbox::KeyCode::Home:
                    if (m_cursor != 0)
                    {
                        m_cursor = 0;
                        actions.markDirty(m_id);
                    }
                    return true;

                case dsbox::KeyCode::End:
                    if (m_cursor != m_text.size())
                    {
                        m_cursor = m_text.size();
                        actions.markDirty(m_id);
                    }
                    return true;

                case dsbox::KeyCode::Enter:
                case dsbox::KeyCode::KeypadEnter:
                    if (!keyDown->repeat && m_onSubmit)
                        m_onSubmit();
                    return true;

                default:
                    // Let it bubble -- e.g. so a parent-level shortcut
                    // still sees a key this field has no use for.
                    return false;
            }
        }

        return false;
    }
}
