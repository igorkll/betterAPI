#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/length.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <string>
#include <vector>

namespace dsbox_ui
{
    // Single-line, non-interactive text leaf: measures itself from
    // IUiRenderBackend::measureText()/shapeText() and paints exactly
    // one GlyphRunCommand. This is v1's one text element -- see
    // Container's own class comment (container.hpp) for the same
    // "future HTML5-like elements arrive as sibling IUiElement
    // implementations" framing this follows.
    //
    // v1 shaping/caching model: measure() is the ONLY place this class
    // calls into `backend` at all -- it calls measureText() (for the
    // UiSize it reports) AND shapeText() (to get the actual positioned
    // glyphs), then caches both results. paint() never touches
    // `backend`; it just copies the cached glyphs into a
    // GlyphRunCommand at whatever baseline arrange()'s bounds() implies
    // -- see GlyphRunCommand's own comment (display_list.hpp) for why
    // that split exists. A consequence: changing setText()/setFont()
    // does NOT repaint anything on its own -- there is no
    // IUiElementActions reachable from outside handleEvent() (see
    // element.hpp), so whatever owns this Text is responsible for
    // calling WindowUiTree::markDirty(id()) itself after reconfiguring
    // it, the same way a WindowUiTree consumer already has to for any
    // other purely-external state change.
    //
    // v3 layout addition: optional greedy word-wrap. setWrapEnabled()
    // defaults to false, which preserves the exact v1 single-line
    // behavior described below. When enabled AND the incoming
    // UiConstraints::maxWidth is finite, measure() instead splits
    // m_text on ASCII spaces and greedily packs words onto lines no
    // wider than maxWidth, reporting a height of lineHeight * number
    // of lines. Known v3 limitations, kept deliberately simple:
    //   - Splitting is ASCII-space-only. No Unicode line-breaking
    //     (UAX #14), no explicit '\n' handling (embedded newlines are
    //     just another non-space byte and will NOT start a new line),
    //     and no hyphenation.
    //   - A single word wider than maxWidth is never broken
    //     mid-word -- it is placed alone on its own (overflowing)
    //     line, same spirit as Container's own "don't shrink below
    //     content" stance elsewhere in this toolkit.
    //   - Every line is assumed to share the same font-level metrics
    //     (ascent/descent/lineHeight) as the first measured line/word,
    //     because TextMetrics conflates font-level and content-level
    //     data in this API and backends are not asked to measure an
    //     empty string separately just to get font metrics.
    // v1 layout limitation (still true when wrap is disabled, i.e. by
    // default): single line only. There is no line-breaking/wrapping
    // logic -- measure() reports exactly one line's width/height
    // (TextMetrics::advanceWidth/lineHeight) clamped into the incoming
    // constraints, same as Container's own Pixel-length behavior,
    // never reflowing the text itself to fit a narrower box.
    //
    // v1 identity limitation: same as Container's own (container.hpp)
    // -- the caller supplies id(), there is no id-minting factory yet.
    class DSBOX_UI_API Text final : public IUiElement
    {
    public:
        explicit Text(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~Text() override;

        Text(const Text&) = delete;
        Text& operator=(const Text&) = delete;

        // -- IUiElement --
        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_idMode; }
        std::string_view name() const noexcept override { return m_name; }

        // Calls backend.measureText()/shapeText() (see this class's
        // own "v1 shaping/caching model" above) and caches both --
        // O(1) after that until the next measure() call, never during
        // arrange()/paint().
        UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) override;

        void arrange(const UiRect& rect, IUiRenderBackend& backend) override;
        UiRect bounds() const noexcept override { return m_bounds; }

        // Emits exactly one GlyphRunCommand from whatever measure()
        // last cached, positioned at this element's own bounds()
        // (baseline = bounds().y + the cached TextMetrics::ascent).
        // Emits nothing at all if measure() has never run, if the last
        // measure() had an empty string, or if setFont() is still
        // kInvalidFontHandle (e.g. loadFont() itself failed -- see
        // IUiRenderBackend::loadFont()'s own doc).
        void paint(DisplayListBuilder& out) const override;

        // Not interactive in v1 -- always returns false. A future
        // text-selection/editing element is a natural follow-up but a
        // deliberately separate concern from this one (mirrors HTML's
        // own split between a plain text node and an <input>/
        // contenteditable element).
        bool handleEvent(const UiEvent&, IUiElementActions&) override { return false; }

        // Always empty -- a leaf, same as Image.
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        // -- Text-specific configuration --

        // UTF-8. Does not itself trigger a re-measure/repaint -- see
        // this class's own "v1 shaping/caching model" above.
        void setText(std::string text) { m_text = std::move(text); }
        const std::string& text() const noexcept { return m_text; }

        // kInvalidFontHandle (the default) means "nothing to draw" --
        // see paint()'s own doc. Set this to whatever
        // IUiRenderBackend::loadFont() returned for the desired
        // face/size; this class never calls loadFont() itself (that
        // would tie every Text instance to loading its own font file,
        // when the whole point of FontHandle is letting several Text
        // elements share one already-loaded font).
        void setFont(FontHandle font) { m_font = font; }
        FontHandle font() const noexcept { return m_font; }

        void setColor(UiColor color) { m_color = color; }

        // See this class's own "v3 layout addition" comment above.
        // Defaults to false (v1 single-line behavior unchanged).
        void setWrapEnabled(bool enabled) { m_wrapEnabled = enabled; }
        bool wrapEnabled() const noexcept { return m_wrapEnabled; }

    private:
        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        std::string m_text;
        FontHandle m_font = kInvalidFontHandle;
        UiColor m_color{ 0.0f, 0.0f, 0.0f, 1.0f };

        bool m_wrapEnabled = false;

        // Cached by the most recent measure() call -- see this class's
        // own "v1 shaping/caching model" above for why paint() reads
        // these rather than re-shaping. One entry per wrapped line;
        // when wrap is disabled (or there's nothing to wrap) this
        // holds exactly one entry, same as v1's single m_shapedGlyphs
        // did -- see "v3 layout addition" above.
        TextMetrics m_metrics;
        std::vector<std::vector<ShapedGlyph>> m_shapedLines;

        UiRect m_bounds;
    };
}
