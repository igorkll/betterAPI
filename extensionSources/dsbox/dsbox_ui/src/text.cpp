#include <dsbox_ui/text.hpp>

#include <algorithm>
#include <cmath>

namespace dsbox_ui
{
    namespace
    {
        // Splits on ASCII space only -- see Text's own "v3 layout
        // addition" doc comment (text.hpp) for why that's a
        // deliberate, documented limitation rather than an oversight.
        // Collapses runs of spaces (consecutive spaces never produce
        // empty words) and drops leading/trailing spaces, matching
        // the natural reading of greedy word-wrap: whitespace is a
        // separator, not content to preserve pixel-for-pixel.
        std::vector<std::string> splitOnAsciiSpace(const std::string& text)
        {
            std::vector<std::string> words;
            std::string current;
            for (char c : text)
            {
                if (c == ' ')
                {
                    if (!current.empty())
                    {
                        words.push_back(std::move(current));
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }
            if (!current.empty())
                words.push_back(std::move(current));
            return words;
        }
    }
    Text::Text(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // Container's destructor -- see that class's own comment.
    Text::~Text() = default;

    UiSize Text::measure(const UiConstraints& constraints, IUiRenderBackend& backend)
    {
        if (m_font == kInvalidFontHandle || m_text.empty())
        {
            // Nothing this element could possibly shape/draw -- cache
            // empty results (rather than whatever measure() last
            // computed) so a Text that had text set, then cleared,
            // doesn't keep painting its old glyphs forever.
            m_metrics = TextMetrics{};
            m_shapedLines.clear();
            return { constraints.clampWidth(0.0f), constraints.clampHeight(0.0f) };
        }

        // Wrap only kicks in when explicitly enabled AND there's a
        // finite width to wrap against -- an unbounded maxWidth (the
        // common case for a Text sized by its own content) has no
        // wrap point to greedily pack against, so it falls through to
        // the same single-line path v1 always used. See Text's own
        // "v3 layout addition" doc comment (text.hpp).
        if (!m_wrapEnabled || !std::isfinite(constraints.maxWidth))
        {
            m_metrics = backend.measureText(m_font, m_text);
            m_shapedLines.assign(1, backend.shapeText(m_font, m_text));

            return {
                constraints.clampWidth(m_metrics.advanceWidth),
                constraints.clampHeight(m_metrics.lineHeight)
            };
        }

        // -- Greedy word-wrap path --
        std::vector<std::string> words = splitOnAsciiSpace(m_text);
        std::vector<std::string> lines;
        std::string currentLine;

        for (const std::string& word : words)
        {
            const std::string candidate = currentLine.empty() ? word : currentLine + " " + word;
            const TextMetrics candidateMetrics = backend.measureText(m_font, candidate);

            if (candidateMetrics.advanceWidth > constraints.maxWidth && !currentLine.empty())
            {
                // Candidate doesn't fit and we already have something
                // on this line -- commit it and start fresh with just
                // `word`. If `word` ALONE is still too wide, it is
                // still placed alone on its own line rather than
                // broken mid-word -- see "v3 layout addition" above.
                lines.push_back(std::move(currentLine));
                currentLine = word;
            }
            else
            {
                currentLine = candidate;
            }
        }
        if (!currentLine.empty())
            lines.push_back(std::move(currentLine));

        // m_text was non-empty and splitOnAsciiSpace() only drops pure
        // whitespace, but guard anyway so a whitespace-only m_text
        // (e.g. " ") still measures/paints as the empty case rather
        // than crashing below on an empty `lines`.
        if (lines.empty())
        {
            m_metrics = TextMetrics{};
            m_shapedLines.clear();
            return { constraints.clampWidth(0.0f), constraints.clampHeight(0.0f) };
        }

        // Font-level metrics (ascent/descent/lineHeight) are assumed
        // constant across lines -- see "v3 layout addition" above for
        // why. Using the first line's measurement is as good as any.
        m_metrics = backend.measureText(m_font, lines.front());

        float maxLineWidth = 0.0f;
        m_shapedLines.clear();
        m_shapedLines.reserve(lines.size());
        for (const std::string& line : lines)
        {
            const TextMetrics lineMetrics = backend.measureText(m_font, line);
            maxLineWidth = std::max(maxLineWidth, lineMetrics.advanceWidth);
            m_shapedLines.push_back(backend.shapeText(m_font, line));
        }

        return {
            constraints.clampWidth(maxLineWidth),
            constraints.clampHeight(m_metrics.lineHeight * static_cast<float>(lines.size()))
        };
    }

    void Text::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    void Text::paint(DisplayListBuilder& out) const
    {
        if (m_font == kInvalidFontHandle || m_shapedLines.empty())
            return;

        // One GlyphRunCommand per wrapped line, stacked downward by
        // m_metrics.lineHeight -- see Text's own "v3 layout addition"
        // doc comment (text.hpp) for why every line shares the same
        // font-level metrics. `i` (not a separate "emitted lines"
        // counter) drives the y offset, so a blank line in the middle
        // of m_shapedLines (not currently producible by measure()'s
        // own splitting, which never emits empty lines, but kept
        // correct defensively) still advances the lines after it to
        // the right position even though it emits no command itself.
        for (std::size_t i = 0; i < m_shapedLines.size(); ++i)
        {
            const std::vector<ShapedGlyph>& lineGlyphs = m_shapedLines[i];
            if (lineGlyphs.empty())
                continue;

            GlyphRunCommand run;
            run.font = m_font;
            run.x = m_bounds.x;
            run.y = m_bounds.y + m_metrics.ascent + static_cast<float>(i) * m_metrics.lineHeight; // baseline, not top-left -- see GlyphRunCommand's own doc
            run.color = m_color;

            run.glyphIds.reserve(lineGlyphs.size());
            run.glyphOffsets.reserve(lineGlyphs.size());

            for (const ShapedGlyph& glyph : lineGlyphs)
            {
                run.glyphIds.push_back(glyph.glyphId);
                run.glyphOffsets.push_back(glyph.xAdvance);
            }

            out.glyphRun(std::move(run));
        }
    }
}
