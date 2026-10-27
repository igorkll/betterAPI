#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/progress_bar.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <variant>
#include <vector>

using namespace dsbox_ui;

namespace
{
    class DummyRenderBackend final : public IUiRenderBackend
    {
    public:
        FontHandle loadFont(std::string_view, float) override { return kInvalidFontHandle; }
        TextMetrics measureText(FontHandle, std::string_view) override { return {}; }
        std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return {}; }
        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    bool sameColor(UiColor a, UiColor b)
    {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    void testSetProgressClampsToUnitRange()
    {
        ProgressBar bar(1);

        bar.setProgress(0.5f);
        DSBOX_UI_CHECK_EQ(bar.progress(), 0.5f);

        bar.setProgress(-1.0f);
        DSBOX_UI_CHECK_EQ(bar.progress(), 0.0f);

        bar.setProgress(5.0f);
        DSBOX_UI_CHECK_EQ(bar.progress(), 1.0f);
    }

    void testDeterminateModeIsDefault()
    {
        ProgressBar bar(1);
        DSBOX_UI_CHECK(bar.mode() == ProgressBarMode::Determinate);

        bar.setMode(ProgressBarMode::Indeterminate);
        DSBOX_UI_CHECK(bar.mode() == ProgressBarMode::Indeterminate);
    }

    void testMeasureReturnsPreferredPixelSize()
    {
        ProgressBar bar(1);
        bar.setPreferredWidth(UiLength::pixels(150.0f));
        bar.setPreferredHeight(UiLength::pixels(10.0f));

        DummyRenderBackend backend;
        const UiSize size = bar.measure(UiConstraints{}, backend);

        DSBOX_UI_CHECK_EQ(size.width, 150.0f);
        DSBOX_UI_CHECK_EQ(size.height, 10.0f);
    }

    void testDeterminatePaintProducesFillProportionalToProgress()
    {
        const UiColor fillColor{ 0.2f, 0.6f, 0.9f, 1.0f };

        ProgressBar bar(1);
        bar.setFillColor(fillColor);
        bar.setProgress(0.5f);

        DummyRenderBackend backend;
        bar.measure(UiConstraints{}, backend);
        bar.arrange(UiRect{ 0.0f, 0.0f, 100.0f, 10.0f }, backend);

        DisplayListBuilder builder;
        bar.paint(builder);
        const DisplayList list = builder.take();

        bool foundHalfWidthFill = false;
        for (const DrawCommand& command : list.commands)
        {
            if (const auto* rect = std::get_if<RectCommand>(&command))
            {
                if (sameColor(rect->color, fillColor) && rect->rect.width == 50.0f)
                    foundHalfWidthFill = true;
            }
        }

        DSBOX_UI_CHECK(foundHalfWidthFill);
    }

    void testHandleEventNeverConsumesAnything()
    {
        // Not interactive in v1 -- see this class's own class comment.
        ProgressBar bar(1);

        UiEvent event{};
        event.target = bar.id();
        event.phase = UiEventPhase::Target;
        event.payload = PointerDownEvent{ 1.0f, 1.0f, dsbox::MouseButton::Left };

        struct NullActions final : IUiElementActions
        {
            void markDirty(UiElementId, UiDirtyLevel = UiDirtyLevel::Layout) override {}
            void requestFocus(UiElementId) override {}
            void setPointerCapture(UiElementId) override {}
            void releasePointerCapture(UiElementId) override {}
        } actions;

        DSBOX_UI_CHECK(!bar.handleEvent(event, actions));
    }

    void testAdvanceDoesNotCrashAndIndeterminatePaints()
    {
        ProgressBar bar(1);
        bar.setMode(ProgressBarMode::Indeterminate);
        bar.advance(0.5f);
        bar.advance(1.5f);

        DummyRenderBackend backend;
        bar.measure(UiConstraints{}, backend);
        bar.arrange(UiRect{ 0.0f, 0.0f, 100.0f, 10.0f }, backend);

        DisplayListBuilder builder;
        bar.paint(builder); // just must not crash/throw
        const DisplayList list = builder.take();
        DSBOX_UI_CHECK(!list.commands.empty());
    }
}

void runProgressBarTests()
{
    dsbox_ui_tests::runTest("SetProgressClampsToUnitRange", testSetProgressClampsToUnitRange);
    dsbox_ui_tests::runTest("DeterminateModeIsDefault", testDeterminateModeIsDefault);
    dsbox_ui_tests::runTest("MeasureReturnsPreferredPixelSize", testMeasureReturnsPreferredPixelSize);
    dsbox_ui_tests::runTest("DeterminatePaintProducesFillProportionalToProgress", testDeterminatePaintProducesFillProportionalToProgress);
    dsbox_ui_tests::runTest("HandleEventNeverConsumesAnything", testHandleEventNeverConsumesAnything);
    dsbox_ui_tests::runTest("AdvanceDoesNotCrashAndIndeterminatePaints", testAdvanceDoesNotCrashAndIndeterminatePaints);
}
