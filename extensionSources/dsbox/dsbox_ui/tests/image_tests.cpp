#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/image.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <cstddef>
#include <variant>

using namespace dsbox_ui;

namespace
{
    // measureText()/loadFont() etc. are never exercised by these tests
    // -- Image never calls into IUiRenderBackend at all (see its own
    // "v1 sizing limitation" comment) -- this backend exists only to
    // satisfy measure()/arrange()'s signature.
    class DummyRenderBackend final : public IUiRenderBackend
    {
    public:
        FontHandle loadFont(std::string_view, float) override { return kInvalidFontHandle; }
        TextMetrics measureText(FontHandle, std::string_view) override { return {}; }
        std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return {}; }
        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    void testMeasureResolvesPixelDimensions()
    {
        Image image(1, UiElementIdMode::Unique, "pic");
        image.setPreferredWidth(UiLength::pixels(50.0f));
        image.setPreferredHeight(UiLength::pixels(30.0f));

        DummyRenderBackend backend;
        UiSize size = image.measure(UiConstraints{}, backend); // unbounded

        DSBOX_UI_CHECK_EQ(size.width, 50.0f);
        DSBOX_UI_CHECK_EQ(size.height, 30.0f);
    }

    void testPaintEmitsNothingWithoutImageSet()
    {
        Image image(1, UiElementIdMode::Unique, "pic");
        image.setPreferredWidth(UiLength::pixels(50.0f));
        image.setPreferredHeight(UiLength::pixels(30.0f));

        DummyRenderBackend backend;
        UiSize size = image.measure(UiConstraints{}, backend);
        image.arrange(UiRect{ 0.0f, 0.0f, size.width, size.height }, backend);

        DisplayListBuilder builder;
        image.paint(builder);
        DisplayList list = builder.take();

        // setImage() was never called -- still kInvalidImageHandle --
        // see paint()'s own doc for why this emits nothing despite
        // having a non-zero area.
        DSBOX_UI_CHECK(list.commands.empty());
    }

    void testPaintEmitsImageCommandCoveringBoundsWhenImageSet()
    {
        Image image(1, UiElementIdMode::Unique, "pic");
        image.setPreferredWidth(UiLength::pixels(50.0f));
        image.setPreferredHeight(UiLength::pixels(30.0f));
        image.setImage(ImageHandle{ 42 });
        image.setTint(UiColor{ 0.5f, 0.6f, 0.7f, 0.8f });

        DummyRenderBackend backend;
        UiSize size = image.measure(UiConstraints{}, backend);
        image.arrange(UiRect{ 3.0f, 4.0f, size.width, size.height }, backend);

        DisplayListBuilder builder;
        image.paint(builder);
        DisplayList list = builder.take();

        DSBOX_UI_CHECK_EQ(list.commands.size(), static_cast<std::size_t>(1));

        const ImageCommand* command = list.commands.empty() ? nullptr : std::get_if<ImageCommand>(&list.commands[0]);
        DSBOX_UI_CHECK(command != nullptr);
        if (command)
        {
            DSBOX_UI_CHECK(command->rect == (UiRect{ 3.0f, 4.0f, 50.0f, 30.0f }));
            DSBOX_UI_CHECK(command->image == ImageHandle{ 42 });
            DSBOX_UI_CHECK(command->tint.r == 0.5f && command->tint.g == 0.6f &&
                           command->tint.b == 0.7f && command->tint.a == 0.8f);
        }
    }

    void testPaintEmitsNothingWithZeroArea()
    {
        Image image(1, UiElementIdMode::Unique, "pic");
        image.setImage(ImageHandle{ 42 });
        // No arrange() call at all -- bounds() defaults to a zero
        // rect, same degenerate case Container::paint() itself guards
        // against.

        DisplayListBuilder builder;
        image.paint(builder);
        DisplayList list = builder.take();

        DSBOX_UI_CHECK(list.commands.empty());
    }
}

void runImageTests()
{
    dsbox_ui_tests::runTest("MeasureResolvesPixelDimensions", testMeasureResolvesPixelDimensions);
    dsbox_ui_tests::runTest("PaintEmitsNothingWithoutImageSet", testPaintEmitsNothingWithoutImageSet);
    dsbox_ui_tests::runTest("PaintEmitsImageCommandCoveringBoundsWhenImageSet", testPaintEmitsImageCommandCoveringBoundsWhenImageSet);
    dsbox_ui_tests::runTest("PaintEmitsNothingWithZeroArea", testPaintEmitsNothingWithZeroArea);
}
