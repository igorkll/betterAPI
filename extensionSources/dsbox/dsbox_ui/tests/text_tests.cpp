#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/text.hpp>

#include <cstddef>
#include <variant>
#include <vector>

using namespace dsbox_ui;

namespace
{
    // Deterministic fake shaping/measurement, standing in for a real
    // font: every byte of the input is treated as one 10px-wide glyph,
    // in an (invented) 12px line with an 8px ascent/4px descent.
    // measureText()/shapeText() are the only two IUiRenderBackend
    // calls Text ever makes (see its own "v1 shaping/caching model"
    // comment) -- both return kInvalidFontHandle/empty for
    // kInvalidFontHandle, mirroring how a real backend's loadFont()
    // failing would look from Text's point of view.
    class TestTextRenderBackend final : public IUiRenderBackend
    {
    public:
        FontHandle loadFont(std::string_view, float) override { return kInvalidFontHandle; }

        TextMetrics measureText(FontHandle font, std::string_view utf8) override
        {
            if (font == kInvalidFontHandle)
                return {};
            return TextMetrics{ 8.0f, 4.0f, 12.0f, static_cast<float>(utf8.size()) * 10.0f };
        }

        std::vector<ShapedGlyph> shapeText(FontHandle font, std::string_view utf8) override
        {
            if (font == kInvalidFontHandle)
                return {};

            std::vector<ShapedGlyph> glyphs;
            glyphs.reserve(utf8.size());
            for (std::size_t i = 0; i < utf8.size(); ++i)
            {
                glyphs.push_back({
                    static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i])),
                    static_cast<float>(i) * 10.0f
                });
            }
            return glyphs;
        }

        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    void testMeasureWithInvalidFontMeasuresAndPaintsAsEmpty()
    {
        Text text(1, UiElementIdMode::Unique, "label");
        text.setText("hello");
        // setFont() never called -- font stays kInvalidFontHandle.

        TestTextRenderBackend backend;
        UiSize size = text.measure(UiConstraints{}, backend);

        DSBOX_UI_CHECK_EQ(size.width, 0.0f);
        DSBOX_UI_CHECK_EQ(size.height, 0.0f);

        text.arrange(UiRect{ 0.0f, 0.0f, size.width, size.height }, backend);

        DisplayListBuilder builder;
        text.paint(builder);
        DisplayList list = builder.take();

        DSBOX_UI_CHECK(list.commands.empty());
    }

    void testMeasureReportsBackendMetricsAndClampsToConstraints()
    {
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 1 });
        text.setText("abc"); // 3 bytes -> advanceWidth 30, lineHeight 12 (see TestTextRenderBackend)

        TestTextRenderBackend backend;

        // Unbounded constraints: measure() reports the backend's own
        // metrics untouched.
        UiSize size = text.measure(UiConstraints{}, backend);
        DSBOX_UI_CHECK_EQ(size.width, 30.0f);
        DSBOX_UI_CHECK_EQ(size.height, 12.0f);

        // A max width narrower than the text's own advance clamps it
        // down, same as any other IUiElement's measure() result.
        UiConstraints narrow{ 0.0f, 20.0f, 0.0f, 20.0f };
        UiSize clamped = text.measure(narrow, backend);
        DSBOX_UI_CHECK_EQ(clamped.width, 20.0f);
        DSBOX_UI_CHECK_EQ(clamped.height, 12.0f); // 12 <= 20, unaffected
    }

    void testPaintEmitsSingleGlyphRunAtBaseline()
    {
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 7 });
        text.setText("ab");
        text.setColor(UiColor{ 0.1f, 0.2f, 0.3f, 1.0f });

        TestTextRenderBackend backend;
        UiSize size = text.measure(UiConstraints{}, backend);
        text.arrange(UiRect{ 5.0f, 10.0f, size.width, size.height }, backend);

        DisplayListBuilder builder;
        text.paint(builder);
        DisplayList list = builder.take();

        DSBOX_UI_CHECK_EQ(list.commands.size(), static_cast<std::size_t>(1));

        const GlyphRunCommand* run = list.commands.empty() ? nullptr : std::get_if<GlyphRunCommand>(&list.commands[0]);
        DSBOX_UI_CHECK(run != nullptr);
        if (run)
        {
            DSBOX_UI_CHECK(run->font == FontHandle{ 7 });
            DSBOX_UI_CHECK_EQ(run->x, 5.0f);
            DSBOX_UI_CHECK_EQ(run->y, 10.0f + 8.0f); // bounds.y + ascent (8, see TestTextRenderBackend)
            DSBOX_UI_CHECK(run->color.r == 0.1f && run->color.g == 0.2f && run->color.b == 0.3f);

            DSBOX_UI_CHECK_EQ(run->glyphIds.size(), static_cast<std::size_t>(2));
            DSBOX_UI_CHECK_EQ(run->glyphOffsets.size(), static_cast<std::size_t>(2));
            if (run->glyphOffsets.size() == 2)
            {
                DSBOX_UI_CHECK_EQ(run->glyphOffsets[0], 0.0f);
                DSBOX_UI_CHECK_EQ(run->glyphOffsets[1], 10.0f);
            }
        }
    }

    void testWrapDisabledByDefaultIgnoresMaxWidth()
    {
        // setWrapEnabled() defaults to false -- a finite maxWidth
        // should NOT cause wrapping unless explicitly opted into, so
        // this must behave identically to the pre-wrap v1 single-line
        // path (one line, full unclamped-by-wrap advanceWidth before
        // the ordinary UiConstraints clamp is applied).
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 1 });
        text.setText("ab cd"); // 5 bytes -> advanceWidth 50 if treated as one line

        TestTextRenderBackend backend;
        UiConstraints narrow{ 0.0f, 25.0f, 0.0f, 1000.0f }; // narrower than the 5-byte line
        UiSize size = text.measure(narrow, backend);

        // Single line, clamped to maxWidth -- never reflowed.
        DSBOX_UI_CHECK_EQ(size.width, 25.0f);
        DSBOX_UI_CHECK_EQ(size.height, 12.0f); // one line's lineHeight, not two
    }

    void testWrapEnabledWithUnboundedWidthStaysSingleLine()
    {
        // Wrap needs a finite maxWidth to wrap against -- an
        // unbounded constraint (the default UiConstraints{}) falls
        // through to the same single-line path as wrap-disabled, per
        // Text's own "v3 layout addition" doc comment.
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 1 });
        text.setText("ab cd ef");
        text.setWrapEnabled(true);

        TestTextRenderBackend backend;
        UiSize size = text.measure(UiConstraints{}, backend);

        DSBOX_UI_CHECK_EQ(size.width, 80.0f); // 8 bytes * 10
        DSBOX_UI_CHECK_EQ(size.height, 12.0f);
    }

    void testWrapEnabledGreedilyPacksWordsAndReportsStackedHeight()
    {
        // Each word below is 2 bytes (20px) and a space-joined "word
        // word" is 5 bytes (50px, since the space is just another
        // glyph byte in TestTextRenderBackend). A maxWidth of 25px
        // fits exactly one word per line (20 <= 25) but never two
        // (50 > 25), so "aa bb cc" greedily wraps into three lines:
        // "aa", "bb", "cc".
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 1 });
        text.setText("aa bb cc");
        text.setWrapEnabled(true);

        TestTextRenderBackend backend;
        UiConstraints wrapping{ 0.0f, 25.0f, 0.0f, 1000.0f };
        UiSize size = text.measure(wrapping, backend);

        DSBOX_UI_CHECK_EQ(size.width, 20.0f);  // widest line ("aa"/"bb"/"cc", all 20px)
        DSBOX_UI_CHECK_EQ(size.height, 36.0f); // 3 lines * 12px lineHeight

        text.arrange(UiRect{ 0.0f, 0.0f, size.width, size.height }, backend);

        DisplayListBuilder builder;
        text.paint(builder);
        DisplayList list = builder.take();

        // One GlyphRunCommand per line.
        DSBOX_UI_CHECK_EQ(list.commands.size(), static_cast<std::size_t>(3));
        for (std::size_t i = 0; i < list.commands.size() && i < 3; ++i)
        {
            const GlyphRunCommand* run = std::get_if<GlyphRunCommand>(&list.commands[i]);
            DSBOX_UI_CHECK(run != nullptr);
            if (run)
            {
                // Each line's baseline is one lineHeight further down
                // than the last -- see Text::paint()'s own comment.
                DSBOX_UI_CHECK_EQ(run->y, 0.0f + 8.0f + static_cast<float>(i) * 12.0f);
                DSBOX_UI_CHECK_EQ(run->glyphIds.size(), static_cast<std::size_t>(2)); // "aa"/"bb"/"cc" are 2 bytes each
            }
        }
    }

    void testWrapEnabledNeverBreaksAWordWiderThanMaxWidth()
    {
        // A single word wider than maxWidth is placed alone on its
        // own (overflowing) line rather than broken mid-word -- see
        // Text's own "v3 layout addition" doc comment.
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 1 });
        text.setText("abcdefghij"); // 10 bytes, one "word", 100px wide
        text.setWrapEnabled(true);

        TestTextRenderBackend backend;
        UiConstraints tiny{ 0.0f, 10.0f, 0.0f, 1000.0f }; // far narrower than the one word
        UiSize size = text.measure(tiny, backend);

        // One line, not broken -- width clamps to maxWidth (10) same
        // as any other over-content measure() clamp, but there is
        // still exactly one line's worth of height.
        DSBOX_UI_CHECK_EQ(size.width, 10.0f);
        DSBOX_UI_CHECK_EQ(size.height, 12.0f);
    }

    void testChangingTextInvalidatesPreviouslyCachedGlyphs()
    {
        // Guards the "cache empty results, not whatever measure() last
        // computed" behavior measure()'s own comment calls out --
        // without it, clearing setText() after a successful measure()
        // would leave paint() still emitting the OLD glyphs forever.
        Text text(1, UiElementIdMode::Unique, "label");
        text.setFont(FontHandle{ 7 });
        text.setText("ab");

        TestTextRenderBackend backend;
        UiSize firstSize = text.measure(UiConstraints{}, backend);
        DSBOX_UI_CHECK(firstSize.width > 0.0f);

        text.setText("");
        UiSize secondSize = text.measure(UiConstraints{}, backend);
        DSBOX_UI_CHECK_EQ(secondSize.width, 0.0f);
        DSBOX_UI_CHECK_EQ(secondSize.height, 0.0f);

        text.arrange(UiRect{ 0.0f, 0.0f, secondSize.width, secondSize.height }, backend);

        DisplayListBuilder builder;
        text.paint(builder);
        DisplayList list = builder.take();

        DSBOX_UI_CHECK(list.commands.empty());
    }
}

void runTextTests()
{
    dsbox_ui_tests::runTest("MeasureWithInvalidFontMeasuresAndPaintsAsEmpty", testMeasureWithInvalidFontMeasuresAndPaintsAsEmpty);
    dsbox_ui_tests::runTest("MeasureReportsBackendMetricsAndClampsToConstraints", testMeasureReportsBackendMetricsAndClampsToConstraints);
    dsbox_ui_tests::runTest("PaintEmitsSingleGlyphRunAtBaseline", testPaintEmitsSingleGlyphRunAtBaseline);
    dsbox_ui_tests::runTest("WrapDisabledByDefaultIgnoresMaxWidth", testWrapDisabledByDefaultIgnoresMaxWidth);
    dsbox_ui_tests::runTest("WrapEnabledWithUnboundedWidthStaysSingleLine", testWrapEnabledWithUnboundedWidthStaysSingleLine);
    dsbox_ui_tests::runTest("WrapEnabledGreedilyPacksWordsAndReportsStackedHeight", testWrapEnabledGreedilyPacksWordsAndReportsStackedHeight);
    dsbox_ui_tests::runTest("WrapEnabledNeverBreaksAWordWiderThanMaxWidth", testWrapEnabledNeverBreaksAWordWiderThanMaxWidth);
    dsbox_ui_tests::runTest("ChangingTextInvalidatesPreviouslyCachedGlyphs", testChangingTextInvalidatesPreviouslyCachedGlyphs);
}
