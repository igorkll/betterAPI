#include "test_framework.hpp"

#include <dsbox_ui/button.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/stack_layout.hpp>
#include <dsbox_ui/window_ui_tree.hpp>

#include <memory>

using namespace dsbox_ui;

namespace
{
    // measureText()/loadFont() etc. are never exercised by these tests
    // -- this backend exists only so WindowUiTree::tick() has
    // something to pass to measure().
    class DummyRenderBackend final : public IUiRenderBackend
    {
    public:
        FontHandle loadFont(std::string_view, float) override { return kInvalidFontHandle; }
        TextMetrics measureText(FontHandle, std::string_view) override { return {}; }
        std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return {}; }
        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    void testMakeButtonAssemblesFocusableClickableContainerWithLabel()
    {
        ButtonParts parts = makeButton(1, 2, "Click me");

        DSBOX_UI_CHECK(parts.root != nullptr);
        DSBOX_UI_CHECK(parts.label != nullptr);
        if (!parts.root || !parts.label)
            return;

        DSBOX_UI_CHECK_EQ(parts.root->id(), static_cast<UiElementId>(1));
        DSBOX_UI_CHECK_EQ(parts.label->id(), static_cast<UiElementId>(2));
        DSBOX_UI_CHECK(parts.label->text() == "Click me");

        // The label is the root's one and only child.
        DSBOX_UI_CHECK_EQ(parts.root->children().size(), static_cast<std::size_t>(1));

        auto* asFocusTarget = dynamic_cast<IFocusTarget*>(parts.root.get());
        DSBOX_UI_CHECK(asFocusTarget != nullptr);
        if (asFocusTarget)
            DSBOX_UI_CHECK(asFocusTarget->isFocusable()); // makeButton() turns this on by default
    }

    void testMakeButtonRootFiresOnClickLikeAnyOtherContainer()
    {
        // makeButton() hands back an ordinary Container as its root --
        // this is really just confirming that fact still holds (no
        // new/different click machinery was introduced), the same way
        // container_tests.cpp's own ClickFiresOnClickOnlyOnMatchingPress
        // test works.
        ButtonParts parts = makeButton(1, 2, "Submit");

        int clickCount = 0;
        parts.root->setOnClick([&clickCount] { ++clickCount; });

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(parts.root));
        DummyRenderBackend backend;

        // Give the button real bounds so the pointer position below
        // actually hits it.
        tree.tick(backend, UiSize{ 200.0f, 200.0f });

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 10.0f, 10.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(down);

        UiEvent up{};
        up.handle = dsbox::WindowHandle{ 1 };
        up.payload = PointerUpEvent{ 10.0f, 10.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(up);

        DSBOX_UI_CHECK_EQ(clickCount, 1);
    }

    // Regression test for the bug fixed this pass: a click that lands
    // on the label's own glyph bounds -- not just the bare background
    // around it -- must still fire the button's onClick. Before this
    // fix, Container::handleEvent() only ever reacted at
    // UiEventPhase::Target with event.target == its own id(); since
    // Text::handleEvent() (text.hpp) always returns false and never
    // changes event.target, a click hit-tested onto the label reached
    // the button Container only at UiEventPhase::Bubble, with target
    // still equal to the LABEL's id -- which the old gate silently
    // dropped. testMakeButtonRootFiresOnClickLikeAnyOtherContainer
    // above doesn't catch this: with DummyRenderBackend's all-zero
    // TextMetrics the label has zero size, so (10, 10) there can only
    // ever hit the button's own background, never the label.
    void testClickOnLabelGlyphsStillFiresButtonOnClick()
    {
        // Unlike DummyRenderBackend elsewhere in this file, this one
        // reports a real (non-invalid) font and non-zero metrics, so
        // the label actually occupies space for hit-testing to land
        // on.
        class LabelRenderBackend final : public IUiRenderBackend
        {
        public:
            FontHandle loadFont(std::string_view, float) override { return 1; }
            TextMetrics measureText(FontHandle, std::string_view) override { return { 16.0f, 4.0f, 20.0f, 60.0f }; }
            std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return { ShapedGlyph{ 1, 0.0f } }; }
            ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
            void submit(const DisplayList&) override {}
        };

        ButtonParts parts = makeButton(1, 2, "Submit");
        parts.label->setFont(1); // non-kInvalidFontHandle -- see Text::setFont()'s own doc

        int clickCount = 0;
        parts.root->setOnClick([&clickCount] { ++clickCount; });

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(parts.root));
        LabelRenderBackend backend;

        // Root fills the whole 200x200 window (see WindowUiTree::
        // tick()'s own root arrange() call); makeButton()'s 16px left
        // padding plus CrossAlign::Center then puts this 60x20 label
        // at roughly x in [16, 76], y in [90, 110] -- see
        // StackLayout::layout()'s own cross-axis-centering math.
        // (21, 100) is well inside the label's glyph bounds, not just
        // the button's background.
        tree.tick(backend, UiSize{ 200.0f, 200.0f });

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 21.0f, 100.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(down);

        UiEvent up{};
        up.handle = dsbox::WindowHandle{ 1 };
        up.payload = PointerUpEvent{ 21.0f, 100.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(up);

        DSBOX_UI_CHECK_EQ(clickCount, 1);
    }
}

void runButtonTests()
{
    dsbox_ui_tests::runTest("MakeButtonAssemblesFocusableClickableContainerWithLabel", testMakeButtonAssemblesFocusableClickableContainerWithLabel);
    dsbox_ui_tests::runTest("MakeButtonRootFiresOnClickLikeAnyOtherContainer", testMakeButtonRootFiresOnClickLikeAnyOtherContainer);
    dsbox_ui_tests::runTest("ClickOnLabelGlyphsStillFiresButtonOnClick", testClickOnLabelGlyphsStillFiresButtonOnClick);
}
