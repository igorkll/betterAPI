#include <dsbox_ui/text_area.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <variant>
#include <vector>

namespace dsbox_ui
{
    TextArea::TextArea(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // TextField's destructor -- see that class's own comment.
    TextArea::~TextArea() = default;

    void TextArea::setText(std::string text)
    {
        m_text = std::move(text);
        m_cursor = m_text.size();
        m_lastWrapWidth = -1.0f; // force rewrap() next measure() -- see its own doc
    }

    std::size_t TextArea::lineIndexForByte(std::size_t byteOffset) const noexcept
    {
        if (m_lines.empty())
            return 0;

        // The last wrapped line whose own start is at or before
        // byteOffset -- correct for a cursor landing exactly on a byte
        // a soft-wrap dropped (a trailing space) or on the '\n' between
        // two hard lines: both cases attribute the cursor to the line
        // BEFORE that byte, i.e. "end of this line", which is the
        // reading a real multi-line editor's own caret gives it.
        std::size_t result = 0;
        for (std::size_t i = 0; i < m_lines.size(); ++i)
        {
            if (m_lines[i].startByte <= byteOffset)
                result = i;
            else
                break;
        }
        return result;
    }

    float TextArea::glyphOffsetForByteInLine(const WrappedLine& line, std::size_t byteOffset) const noexcept
    {
        if (line.glyphs.empty())
            return 0.0f;

        const std::size_t lineLength = line.endByte - line.startByte;
        std::size_t local = byteOffset > line.startByte ? byteOffset - line.startByte : 0;
        local = std::min(local, lineLength);

        if (local < line.glyphs.size())
            return line.glyphs[local].xAdvance;
        return line.width; // caret after the last glyph on this line
    }

    std::size_t TextArea::byteOffsetForLocalXInLine(const WrappedLine& line, float localX) const noexcept
    {
        if (line.glyphs.empty())
            return line.startByte;

        std::size_t best = 0;
        float bestDistance = std::fabs(localX - glyphOffsetForByteInLine(line, line.startByte));

        for (std::size_t i = 1; i <= line.glyphs.size(); ++i)
        {
            const float distance = std::fabs(localX - glyphOffsetForByteInLine(line, line.startByte + i));
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = i;
            }
        }

        return line.startByte + best;
    }

    void TextArea::moveCursorVertically(int lineDelta)
    {
        if (m_lines.empty())
            return;

        const std::size_t currentIndex = lineIndexForByte(m_cursor);
        const float targetX = glyphOffsetForByteInLine(m_lines[currentIndex], m_cursor);

        const long targetIndexSigned = static_cast<long>(currentIndex) + lineDelta;

        if (targetIndexSigned < 0)
        {
            m_cursor = 0;
            return;
        }

        if (static_cast<std::size_t>(targetIndexSigned) >= m_lines.size())
        {
            m_cursor = m_text.size();
            return;
        }

        m_cursor = byteOffsetForLocalXInLine(m_lines[static_cast<std::size_t>(targetIndexSigned)], targetX);
    }

    // Recomputes m_lines (and m_metrics/m_placeholderGlyphs) for the
    // current m_text wrapped to `maxWidth` -- see this class's own "v1
    // scope" (text_area.hpp) for exactly what "wrapped" means here:
    // '\n' bytes in m_text are hard breaks, each hard line is then
    // independently soft-wrapped at word boundaries, falling back to a
    // mid-word break only when a single word alone exceeds maxWidth.
    void TextArea::rewrap(IUiRenderBackend& backend, float maxWidth)
    {
        m_lines.clear();
        m_placeholderGlyphs.clear();
        m_lastWrapWidth = maxWidth;

        if (m_font == kInvalidFontHandle)
        {
            m_metrics = TextMetrics{};
            return;
        }

        // Stable probe for ascent/descent/lineHeight, same "measure
        // SOMETHING so the caret has a sensible height" reasoning
        // TextField::measure() documents for its own no-text case --
        // here it doubles as every wrapped line's shared line metrics,
        // since every line is set in the same font/size.
        m_metrics = backend.measureText(m_font, std::string_view(" "));

        maxWidth = std::max(1.0f, maxWidth);

        if (m_text.empty())
        {
            m_lines.push_back(WrappedLine{ 0, 0, 0.0f, {} });

            if (!m_placeholder.empty())
                m_placeholderGlyphs = backend.shapeText(m_font, m_placeholder);

            return;
        }

        // Appends one or more WrappedLine entries covering `hardText`
        // (already excluding its own delimiting '\n', if any) to
        // m_lines, breaking it to fit `maxWidth`. `globalStart` is
        // hardText's own first byte's offset into m_text, so every
        // WrappedLine this emits ends up in m_text's own byte
        // coordinates, matching m_cursor -- not local to this one hard
        // line. A lambda (rather than a free function in this .cpp's
        // anonymous namespace) specifically so it can name
        // TextArea::WrappedLine, a private nested type -- only a
        // member (or something compiled as part of a member function
        // body, which a lambda defined here is) may do that.
        auto wrapHardLine = [&](std::string_view hardText, std::size_t globalStart)
        {
            bool emittedAny = false;

            auto emitLine = [&](std::size_t start, std::size_t end)
            {
                const std::string_view sub = hardText.substr(start, end - start);
                WrappedLine line;
                line.startByte = globalStart + start;
                line.endByte = globalStart + end;
                line.width = sub.empty() ? 0.0f : backend.measureText(m_font, sub).advanceWidth;
                line.glyphs = sub.empty() ? std::vector<ShapedGlyph>{} : backend.shapeText(m_font, sub);
                m_lines.push_back(std::move(line));
                emittedAny = true;
            };

            if (hardText.empty())
            {
                emitLine(0, 0);
                return;
            }

            const std::vector<ShapedGlyph> wide = backend.shapeText(m_font, hardText);
            const TextMetrics wideMetrics = backend.measureText(m_font, hardText);
            const std::size_t n = hardText.size();

            auto xAt = [&](std::size_t k) -> float
            {
                if (k >= n)
                    return wideMetrics.advanceWidth;
                return wide[k].xAdvance;
            };

            // Alternating runs of non-space / space bytes -- the units
            // word-wrap actually breaks between. Each space run counts
            // as its own token purely so the word-fitting loop below
            // can skip over it without special-casing run length; only
            // word tokens are ever tested against maxWidth.
            struct Token
            {
                std::size_t start, end;
                bool isSpace;
            };

            std::vector<Token> tokens;
            {
                std::size_t i = 0;
                while (i < n)
                {
                    const bool isSpace = hardText[i] == ' ';
                    std::size_t j = i;
                    while (j < n && (hardText[j] == ' ') == isSpace)
                        ++j;
                    tokens.push_back({ i, j, isSpace });
                    i = j;
                }
            }

            std::size_t lineStart = 0;
            std::size_t lastWordEnd = 0;
            bool lineHasWord = false;

            std::size_t tIdx = 0;
            while (tIdx < tokens.size())
            {
                const Token& tok = tokens[tIdx];

                if (tok.isSpace)
                {
                    ++tIdx;
                    continue; // width is checked against the NEXT word, which already includes this space via xAt(lineStart)
                }

                const float widthWithWord = xAt(tok.end) - xAt(lineStart);

                if (widthWithWord <= maxWidth)
                {
                    lastWordEnd = tok.end;
                    lineHasWord = true;
                    ++tIdx;
                    continue;
                }

                if (lineHasWord)
                {
                    // This word doesn't fit, but something else on this
                    // line already does -- close the line at the last
                    // word that fit (dropping any trailing space, same
                    // as a real word-wrap), start a fresh line AT this
                    // word (dropping the leading space(s) before it
                    // too), and retry the SAME word against the new,
                    // empty line.
                    emitLine(lineStart, lastWordEnd);
                    lineStart = tok.start;
                    lineHasWord = false;
                    lastWordEnd = lineStart;
                    continue;
                }

                // Nothing on this line yet and the word alone still
                // doesn't fit -- maxWidth itself is narrower than this
                // one word. Force a mid-word break, repeatedly, until
                // the whole word is consumed; always makes at least
                // one glyph of progress per emitted line so this can
                // never loop forever even if maxWidth is narrower than
                // a single glyph.
                std::size_t splitStart = tok.start;
                while (splitStart < tok.end)
                {
                    std::size_t splitEnd = splitStart;
                    std::size_t k = splitStart + 1;
                    while (k <= tok.end && (xAt(k) - xAt(lineStart)) <= maxWidth)
                    {
                        splitEnd = k;
                        ++k;
                    }
                    if (splitEnd == splitStart)
                        splitEnd = splitStart + 1;

                    emitLine(lineStart, splitEnd);
                    lineStart = splitEnd;
                    splitStart = splitEnd;
                }

                lastWordEnd = lineStart;
                lineHasWord = false;
                ++tIdx;
            }

            if (lineStart < n || !emittedAny)
                emitLine(lineStart, n);
        };

        std::size_t hardStart = 0;
        for (std::size_t i = 0; i <= m_text.size(); ++i)
        {
            if (i == m_text.size() || m_text[i] == '\n')
            {
                wrapHardLine(std::string_view(m_text).substr(hardStart, i - hardStart), hardStart);
                hardStart = i + 1;
            }
        }
    }

    UiSize TextArea::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        const float resolvedWidth = resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth);
        const float wrapWidth = std::max(1.0f, resolvedWidth - 2.0f * kInset);

        if (m_font == kInvalidFontHandle)
        {
            m_metrics = TextMetrics{};
            m_lines.clear();
            m_placeholderGlyphs.clear();
            m_lastWrapWidth = wrapWidth;
        }
        else if (wrapWidth != m_lastWrapWidth)
        {
            rewrap(backend, wrapWidth);
        }

        const float resolvedHeight = resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight);

        return { resolvedWidth, resolvedHeight };
    }

    void TextArea::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    UiRect TextArea::textInputRect() const noexcept
    {
        const float lineHeight = std::max(1.0f, m_metrics.lineHeight);

        if (m_lines.empty())
            return UiRect{ m_bounds.x + kInset, m_bounds.y + kInset, 2.0f, lineHeight };

        const std::size_t lineIdx = lineIndexForByte(m_cursor);
        const WrappedLine& line = m_lines[lineIdx];

        const float caretX = m_bounds.x + kInset + glyphOffsetForByteInLine(line, m_cursor);
        const float caretY = m_bounds.y + kInset + static_cast<float>(lineIdx) * (lineHeight + m_lineSpacing);
        return UiRect{ caretX, caretY, 2.0f, lineHeight };
    }

    void TextArea::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;
        if (!hasArea)
            return;

        // Faked border, same two-nested-rounded-rects trick TextField's
        // own paint() uses -- see its comment (text_field.cpp) for why.
        const UiColor& border = m_focused ? m_focusedBorderColor : m_borderColor;
        out.rect(m_bounds, border, m_cornerRadius);

        constexpr float kBorderWidth = 1.5f;
        const UiRect inner{
            m_bounds.x + kBorderWidth, m_bounds.y + kBorderWidth,
            std::max(0.0f, m_bounds.width - 2.0f * kBorderWidth),
            std::max(0.0f, m_bounds.height - 2.0f * kBorderWidth)
        };

        if (inner.width > 0.0f && inner.height > 0.0f)
            out.rect(inner, m_backgroundColor, std::max(0.0f, m_cornerRadius - kBorderWidth));

        // Content taller than this element's own bounds simply paints
        // past the bottom edge -- clipped here to bounds(), same "no
        // scrolling of its own" v1 note as text_area.hpp's class
        // comment. Wrap this element in a ScrollView for one that
        // scrolls.
        out.pushClip(m_bounds);

        const float lineHeight = std::max(1.0f, m_metrics.lineHeight);
        const float textOriginX = m_bounds.x + kInset;
        const float firstBaselineY = m_bounds.y + kInset + m_metrics.ascent;

        auto emitGlyphRun = [&](const std::vector<ShapedGlyph>& glyphs, float baselineY, UiColor color)
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
            {
                for (std::size_t i = 0; i < m_lines.size(); ++i)
                {
                    emitGlyphRun(
                        m_lines[i].glyphs,
                        firstBaselineY + static_cast<float>(i) * (lineHeight + m_lineSpacing),
                        m_textColor);
                }
            }
            else
            {
                emitGlyphRun(m_placeholderGlyphs, firstBaselineY, m_placeholderColor);
            }
        }

        if (m_focused && !m_lines.empty())
        {
            const std::size_t lineIdx = lineIndexForByte(m_cursor);
            const WrappedLine& line = m_lines[lineIdx];
            const float caretX = textOriginX + glyphOffsetForByteInLine(line, m_cursor);
            const float caretY = m_bounds.y + kInset + static_cast<float>(lineIdx) * (lineHeight + m_lineSpacing);
            out.rect(UiRect{ caretX, caretY, 1.5f, lineHeight }, m_textColor);
        }

        out.popClip();
    }

    bool TextArea::handleEvent(const UiEvent& event, IUiElementActions& actions)
    {
        // Same "only react when THIS element is exactly what the
        // interaction is about" guard Container/TextField's own
        // handleEvent() use.
        if (event.phase != UiEventPhase::Target || event.target != m_id)
            return false;

        if (const auto* down = std::get_if<PointerDownEvent>(&event.payload))
        {
            actions.requestFocus(m_id);

            if (!m_lines.empty())
            {
                const float lineHeight = std::max(1.0f, m_metrics.lineHeight);
                const float localY = down->y - (m_bounds.y + kInset);
                long lineIdx = static_cast<long>(localY / (lineHeight + m_lineSpacing));
                lineIdx = std::clamp<long>(lineIdx, 0, static_cast<long>(m_lines.size()) - 1);

                const WrappedLine& line = m_lines[static_cast<std::size_t>(lineIdx)];
                const float localX = down->x - (m_bounds.x + kInset);
                m_cursor = byteOffsetForLocalXInLine(line, localX);
            }

            actions.markDirty(m_id);
            return true;
        }

        if (std::get_if<PointerUpEvent>(&event.payload))
            return true; // consumed so it doesn't fall through to whatever's behind this field

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
            m_lastWrapWidth = -1.0f; // force rewrap() next measure()
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
                        m_text.erase(m_cursor - 1, 1);
                        --m_cursor;
                        m_lastWrapWidth = -1.0f;
                        actions.markDirty(m_id);
                        if (m_onChange)
                            m_onChange(m_text);
                    }
                    return true;

                case dsbox::KeyCode::Delete:
                    if (m_cursor < m_text.size())
                    {
                        m_text.erase(m_cursor, 1);
                        m_lastWrapWidth = -1.0f;
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

                case dsbox::KeyCode::Up:
                    moveCursorVertically(-1);
                    actions.markDirty(m_id);
                    return true;

                case dsbox::KeyCode::Down:
                    moveCursorVertically(1);
                    actions.markDirty(m_id);
                    return true;

                case dsbox::KeyCode::Home:
                    if (!m_lines.empty())
                    {
                        const WrappedLine& line = m_lines[lineIndexForByte(m_cursor)];
                        if (m_cursor != line.startByte)
                        {
                            m_cursor = line.startByte;
                            actions.markDirty(m_id);
                        }
                    }
                    return true;

                case dsbox::KeyCode::End:
                    if (!m_lines.empty())
                    {
                        const WrappedLine& line = m_lines[lineIndexForByte(m_cursor)];
                        if (m_cursor != line.endByte)
                        {
                            m_cursor = line.endByte;
                            actions.markDirty(m_id);
                        }
                    }
                    return true;

                case dsbox::KeyCode::Enter:
                case dsbox::KeyCode::KeypadEnter:
                    // Unlike TextField (where Enter fires onSubmit()
                    // and never touches the value), Enter here inserts
                    // a hard line break -- the usual <textarea>
                    // behavior. There is deliberately no onSubmit()
                    // equivalent on this class.
                    m_text.insert(m_cursor, 1, '\n');
                    ++m_cursor;
                    m_lastWrapWidth = -1.0f;
                    actions.markDirty(m_id);
                    if (m_onChange)
                        m_onChange(m_text);
                    return true;

                default:
                    return false; // let it bubble -- same reasoning as TextField's own default case
            }
        }

        return false;
    }
}
