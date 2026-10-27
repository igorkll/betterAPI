#include "test_framework.hpp"

#include <dsbox_ui/container.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/stack_layout.hpp>
#include <dsbox_ui/text.hpp>
#include <dsbox_ui/window_ui_tree.hpp>

#include <memory>
#include <variant>

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

    // Every test below builds a tree whose root is one childless
    // Container, so a freshly tick()ed DisplayList should hold exactly
    // one RectCommand -- the root's own background fill.
    const RectCommand* soleRectCommand(const DisplayList& list)
    {
        if (list.commands.size() != 1)
            return nullptr;
        return std::get_if<RectCommand>(&list.commands[0]);
    }

    bool sameColor(UiColor a, UiColor b)
    {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    void testClickFiresOnClickOnlyOnMatchingPress()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");

        int clickCount = 0;
        root->setOnClick([&clickCount] { ++clickCount; });

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 10.0f, 10.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(down);

        DSBOX_UI_CHECK_EQ(clickCount, 0); // a press alone never fires a click

        UiEvent up{};
        up.handle = dsbox::WindowHandle{ 1 };
        up.payload = PointerUpEvent{ 10.0f, 10.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(up);

        DSBOX_UI_CHECK_EQ(clickCount, 1); // press-then-release fires exactly once

        // A release with no matching press before it does not fire
        // again -- see setOnClick()'s own doc on what "matching" means.
        tree.dispatchEvent(up);
        DSBOX_UI_CHECK_EQ(clickCount, 1);
    }

    void testFocusableRequestsFocusOnPress()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setFocusable(true);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), kInvalidUiElementId);

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(down);

        // This is the whole point of WindowUiTree::m_treeMutex being a
        // recursive_mutex: Container::handleEvent() called
        // actions.requestFocus() from INSIDE dispatchEvent()'s own
        // locked scope, above -- if that deadlocked, this test would
        // hang rather than fail cleanly.
        DSBOX_UI_CHECK_EQ(tree.focusedElement(), static_cast<UiElementId>(1));
    }

    void testPressedColorOverridesBackgroundWhilePressed()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        const UiColor normalColor{ 0.2f, 0.2f, 0.2f, 1.0f };
        const UiColor pressedColor{ 0.8f, 0.1f, 0.1f, 1.0f };
        root->setBackgroundColor(normalColor);
        root->setPressedColor(pressedColor);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        const RectCommand* before = soleRectCommand(*tree.currentDisplayList());
        DSBOX_UI_CHECK(before != nullptr);
        if (before)
            DSBOX_UI_CHECK(sameColor(before->color, normalColor));

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(down); // Container::handleEvent() calls actions.markDirty()

        tree.tick(backend, UiSize{ 100.0f, 100.0f }); // markDirty() above should make this actually relayout+repaint
        const RectCommand* pressed = soleRectCommand(*tree.currentDisplayList());
        DSBOX_UI_CHECK(pressed != nullptr);
        if (pressed)
            DSBOX_UI_CHECK(sameColor(pressed->color, pressedColor));

        UiEvent up{};
        up.handle = dsbox::WindowHandle{ 1 };
        up.payload = PointerUpEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(up);

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        const RectCommand* after = soleRectCommand(*tree.currentDisplayList());
        DSBOX_UI_CHECK(after != nullptr);
        if (after)
            DSBOX_UI_CHECK(sameColor(after->color, normalColor));
    }
    void testOpacityScalesFillAlpha()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setBackgroundColor(UiColor{ 0.5f, 0.5f, 0.5f, 1.0f });
        root->setOpacity(0.5f);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;
        tree.tick(backend, UiSize{ 100.0f, 100.0f });

        const RectCommand* rect = soleRectCommand(*tree.currentDisplayList());
        DSBOX_UI_CHECK(rect != nullptr);
        if (rect)
            DSBOX_UI_CHECK(rect->color.a > 0.49f && rect->color.a < 0.51f); // 1.0 background alpha * 0.5 opacity
    }

    void testOpacityIsClampedToUnitRange()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setOpacity(3.0f); // out of range -- must clamp, not reject or overflow the final alpha
        DSBOX_UI_CHECK(root->opacity() == 1.0f);

        root->setOpacity(-1.0f);
        DSBOX_UI_CHECK(root->opacity() == 0.0f);
    }

    // Regression/integration test for WindowUiTree's hover tracking
    // (updateHoverState(), window_ui_tree.cpp) actually reaching
    // Container::setHoverColor() end to end -- container_tests.cpp's
    // own PressedColorOverridesBackgroundWhilePressed test is the
    // :active equivalent of this for :hover.
    void testHoverColorAppliesOnPointerEnterAndClearsOnLeave()
    {
        // The window ROOT itself is always part of every dispatched
        // pointer event's hit path (WindowUiTree::dispatchEvent()'s own
        // hitTestPath() includes the node it starts from unconditionally,
        // with no containment check against that starting node), so it
        // can never actually see a PointerLeaveEvent while this window's
        // tree is live. A CHILD positioned within the root is what
        // demonstrates hover actually clearing once the pointer moves
        // off of it while staying inside the root.
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setPreferredWidth(UiLength::pixels(100.0f));
        root->setPreferredHeight(UiLength::pixels(100.0f));
        root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 0.0f, CrossAlign::Start));

        auto& target = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(2, UiElementIdMode::Unique, "target")));
        target.setPreferredWidth(UiLength::pixels(50.0f));
        target.setPreferredHeight(UiLength::pixels(50.0f));

        const UiColor normalColor{ 0.2f, 0.2f, 0.2f, 1.0f };
        const UiColor hoverColor{ 0.1f, 0.6f, 0.9f, 1.0f };
        target.setBackgroundColor(normalColor);
        target.setHoverColor(hoverColor);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;
        tree.tick(backend, UiSize{ 100.0f, 100.0f });

        // Target ends up at bounds {0,0,50,50} -- CrossAlign::Start
        // puts it at the content rect's own origin, StackLayout's
        // Pass 1 sizes it from its own Pixel(50) measure() result.
        const UiRect targetBounds = tree.root().children()[0]->bounds();
        DSBOX_UI_CHECK(targetBounds == (UiRect{ 0.0f, 0.0f, 50.0f, 50.0f }));

        // The root itself never emits a RectCommand (its own
        // background stays transparent -- alpha 0 -- throughout this
        // test, so Container::paint()'s own "hasArea && fillColor.a >
        // 0" check skips it), so the target's own rect is always the
        // sole command.
        auto findTargetRect = [&]() -> const RectCommand*
        {
            return soleRectCommand(*tree.currentDisplayList());
        };

        const RectCommand* before = findTargetRect();
        DSBOX_UI_CHECK(before != nullptr);
        if (before)
            DSBOX_UI_CHECK(sameColor(before->color, normalColor));

        // Inside target's own bounds {0,0,50,50}.
        UiEvent moveIn{};
        moveIn.handle = dsbox::WindowHandle{ 1 };
        moveIn.payload = PointerMoveEvent{ 10.0f, 10.0f, 0.0f, 0.0f };
        tree.dispatchEvent(moveIn);

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        const RectCommand* hovered = findTargetRect();
        DSBOX_UI_CHECK(hovered != nullptr);
        if (hovered)
            DSBOX_UI_CHECK(sameColor(hovered->color, hoverColor));

        // Still inside the ROOT (100x100) but outside target's own
        // {0,0,50,50} -- hover must clear even though the pointer never
        // left the window.
        UiEvent moveOut{};
        moveOut.handle = dsbox::WindowHandle{ 1 };
        moveOut.payload = PointerMoveEvent{ 80.0f, 80.0f, 0.0f, 0.0f };
        tree.dispatchEvent(moveOut);

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        const RectCommand* after = findTargetRect();
        DSBOX_UI_CHECK(after != nullptr);
        if (after)
            DSBOX_UI_CHECK(sameColor(after->color, normalColor));
    }

    // Regression test for Container::handleEvent()'s own isBubble/
    // interactive guard (container.cpp). Without that guard, making
    // Bubble phase react at all (the fix for "clicking a button's own
    // label does nothing" -- see button_tests.cpp's
    // ClickOnLabelGlyphsStillFiresButtonOnClick) would make EVERY
    // Container ancestor on a bubble path swallow a click just by
    // being a Container -- including a plain, unconfigured wrapper
    // with no onClick and not focusable, which must stay completely
    // transparent instead (the equivalent of a <div> with neither
    // onclick nor tabindex). Builds the same "Container + non-
    // interactive Text leaf" shape makeButton() does, but WITHOUT
    // calling setFocusable()/setOnClick() on the wrapper, so nothing
    // along this path is actually interactive at all.
    void testNonInteractiveWrapperDoesNotSwallowBubbledEvent()
    {
        class LabelRenderBackend final : public IUiRenderBackend
        {
        public:
            FontHandle loadFont(std::string_view, float) override { return 1; }
            TextMetrics measureText(FontHandle, std::string_view) override { return { 16.0f, 4.0f, 20.0f, 60.0f }; }
            std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return { ShapedGlyph{ 1, 0.0f } }; }
            ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
            void submit(const DisplayList&) override {}
        };

        auto wrapper = std::make_unique<Container>(1, UiElementIdMode::Unique, "wrapper");
        wrapper->setPreferredWidth(UiLength::pixels(100.0f));
        wrapper->setPreferredHeight(UiLength::pixels(100.0f));
        wrapper->setPadding(0.0f, 16.0f, 0.0f, 16.0f);
        wrapper->setLayoutStrategy(
            std::make_unique<StackLayout>(StackDirection::Horizontal, 0.0f, CrossAlign::Center));
        // Deliberately NOT focusable, NOT given an onClick -- a pure
        // layout wrapper, not a button.

        auto& label = static_cast<Text&>(
            wrapper->addChild(std::make_unique<Text>(2, UiElementIdMode::Unique, "label")));
        label.setText("Not a button");
        label.setFont(1);

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(wrapper));
        LabelRenderBackend backend;
        tree.tick(backend, UiSize{ 200.0f, 200.0f }); // same geometry button_tests.cpp's own label test uses

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 21.0f, 100.0f, dsbox::MouseButton::Left }; // inside the label's own glyph bounds
        UiEvent resolvedDown = tree.dispatchEvent(down);

        DSBOX_UI_CHECK_EQ(resolvedDown.target, static_cast<UiElementId>(2)); // hit the label
        // Text::handleEvent() always returns false, and the wrapper
        // must now ALSO decline (it isn't interactive) -- nothing on
        // this path consumes the event at all.
        DSBOX_UI_CHECK(!resolvedDown.stopPropagation);
    }

    // Container::measure() content-sizing integration test (the actual
    // sizing logic lives in StackLayout::measureContent(), exercised
    // directly in stack_layout_tests.cpp -- this checks Container
    // actually wires UiUnit::Content into it, padding included).
    void testContentSizedContainerFitsItsChildren()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setPreferredWidth(UiLength::content());
        root->setPreferredHeight(UiLength::content());
        root->setPadding(5.0f, 5.0f, 5.0f, 5.0f); // top, right, bottom, left
        root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 2.0f));

        auto& childA = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(2, UiElementIdMode::Unique, "a")));
        childA.setPreferredWidth(UiLength::pixels(30.0f));
        childA.setPreferredHeight(UiLength::pixels(10.0f));

        auto& childB = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(3, UiElementIdMode::Unique, "b")));
        childB.setPreferredWidth(UiLength::pixels(50.0f)); // widest -- decides content width
        childB.setPreferredHeight(UiLength::pixels(20.0f));

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;
        tree.tick(backend, UiSize{ 500.0f, 500.0f }); // plenty of room -- Content must fit, not fill

        const UiRect bounds = tree.root().bounds();
        // width: padding(5+5) + widest child(50) = 60
        DSBOX_UI_CHECK_EQ(bounds.width, 60.0f);
        // height: padding(5+5) + 10 + gap(2) + 20 = 42
        DSBOX_UI_CHECK_EQ(bounds.height, 42.0f);
    }

    // Minimal leaf IUiElement used only by
    // testIncrementalRelayoutSkipsUntouchedContainerSubtree() below --
    // counts measure() calls so that test can observe, from OUTSIDE
    // WindowUiTree/Container entirely, whether a given subtree's
    // measure() pass actually ran or was skipped. Deliberately does
    // NOT override IUiElement::prepareIncrementalRelayout()/
    // forceFullRelayout() -- the base class's own default
    // (element.hpp) is already correct for any leaf (children().empty()
    // is true), which is exactly the case this is meant to exercise
    // alongside Container's own override.
    class CountingLeaf final : public IUiElement
    {
    public:
        explicit CountingLeaf(UiElementId id) : m_id(id) {}

        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return UiElementIdMode::Unique; }

        UiSize measure(const UiConstraints&, IUiRenderBackend&) override
        {
            ++measureCount;
            return { 10.0f, 10.0f };
        }

        void arrange(const UiRect& rect, IUiRenderBackend&) override { m_bounds = rect; }
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder&) const override {}
        bool handleEvent(const UiEvent&, IUiElementActions&) override { return false; }
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        int measureCount = 0;

    private:
        UiElementId m_id;
        UiRect m_bounds;
    };

    // Regression test for Container::prepareIncrementalRelayout()/
    // Container::measure()'s own cache check (container.cpp): a
    // Container subtree that nothing inside it was markDirty()'d
    // against must have its measure() pass skipped ENTIRELY on the
    // next tick() -- not just inside that subtree's own root, but all
    // the way down to its own leaf children -- even though a SIBLING
    // subtree being dirty still forces the common ancestor (the
    // overall root) to re-run its own measure()/layout pass. The
    // leaf's own measureCount is the only way to observe this from
    // outside the toolkit: TestElement (window_ui_tree_tests.cpp)
    // can't be reused here because it always reports itself dirty (it
    // has children, via its own addChild(), and never overrides
    // prepareIncrementalRelayout() to say otherwise) -- CountingLeaf
    // above exists specifically because it has NO children and so
    // correctly benefits from IUiElement's own default.
    void testIncrementalRelayoutSkipsUntouchedContainerSubtree()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setLayoutStrategy(std::make_unique<StackLayout>(StackDirection::Vertical, 0.0f));

        auto& childA = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(2, UiElementIdMode::Unique, "a")));
        childA.setPreferredWidth(UiLength::pixels(10.0f));
        childA.setPreferredHeight(UiLength::pixels(10.0f));
        auto& leafA = static_cast<CountingLeaf&>(childA.addChild(std::make_unique<CountingLeaf>(3)));

        auto& childB = static_cast<Container&>(
            root->addChild(std::make_unique<Container>(4, UiElementIdMode::Unique, "b")));
        childB.setPreferredWidth(UiLength::pixels(10.0f));
        childB.setPreferredHeight(UiLength::pixels(10.0f));
        auto& leafB = static_cast<CountingLeaf&>(childB.addChild(std::make_unique<CountingLeaf>(5)));

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));
        DummyRenderBackend backend;

        tree.tick(backend, UiSize{ 100.0f, 100.0f });
        DSBOX_UI_CHECK(leafA.measureCount > 0);
        DSBOX_UI_CHECK(leafB.measureCount > 0);
        const int leafBBaseline = leafB.measureCount;

        // Dirty only childA (via its own CONTAINER id, not the leaf's
        // -- Container::measure()'s cache check is what actually gets
        // exercised here). Same window size, so this is NOT a
        // forceFullRelayout() resize pass -- see tick()'s own doc.
        tree.markDirty(childA.id());
        tree.tick(backend, UiSize{ 100.0f, 100.0f });

        // childA's own subtree DID re-measure...
        DSBOX_UI_CHECK(leafA.measureCount > 1);
        // ...but childB's subtree did NOT, even though the shared
        // root ABOVE both of them was forced to re-run its own
        // measure()/layout pass (because childA, its other child, WAS
        // dirty) -- this is the actual per-subtree scoping this test
        // exists to prove.
        DSBOX_UI_CHECK_EQ(leafB.measureCount, leafBBaseline);
    }

    void testIsFocusableReflectsSetFocusable()
    {
        // dynamic_cast<IFocusTarget*> is exactly how WindowUiTree's Tab
        // traversal (see window_ui_tree.cpp's own collectFocusables())
        // decides whether a given element is a stop at all -- this
        // guards that Container actually implements the interface, not
        // just setFocusable()/isFocusable() as unrelated members.
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        auto* asFocusTarget = dynamic_cast<IFocusTarget*>(root.get());

        DSBOX_UI_CHECK(asFocusTarget != nullptr);
        if (!asFocusTarget)
            return;

        DSBOX_UI_CHECK(!asFocusTarget->isFocusable()); // false by default, matches setFocusable()'s own doc

        root->setFocusable(true);
        DSBOX_UI_CHECK(asFocusTarget->isFocusable());

        root->setFocusable(false);
        DSBOX_UI_CHECK(!asFocusTarget->isFocusable());
    }

    void testEnterAndSpaceActivateFocusedClickable()
    {
        auto root = std::make_unique<Container>(1, UiElementIdMode::Unique, "root");
        root->setFocusable(true);

        int clickCount = 0;
        root->setOnClick([&clickCount] { ++clickCount; });

        WindowUiTree tree(dsbox::WindowHandle{ 1 }, std::move(root));

        UiEvent down{};
        down.handle = dsbox::WindowHandle{ 1 };
        down.payload = PointerDownEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(down); // click-to-focus

        UiEvent up{};
        up.handle = dsbox::WindowHandle{ 1 };
        up.payload = PointerUpEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left };
        tree.dispatchEvent(up);
        DSBOX_UI_CHECK_EQ(clickCount, 1); // sanity: the pointer path itself still works

        UiEvent enterDown{};
        enterDown.handle = dsbox::WindowHandle{ 1 };
        enterDown.payload = KeyDownEvent{ dsbox::KeyCode::Enter, dsbox::KeyModifier::None, false };
        tree.dispatchEvent(enterDown);
        DSBOX_UI_CHECK_EQ(clickCount, 2);

        // OS autorepeat does NOT fire onClick() again and again --
        // see Container::handleEvent()'s own comment on this.
        UiEvent enterRepeat{};
        enterRepeat.handle = dsbox::WindowHandle{ 1 };
        enterRepeat.payload = KeyDownEvent{ dsbox::KeyCode::Enter, dsbox::KeyModifier::None, true };
        tree.dispatchEvent(enterRepeat);
        DSBOX_UI_CHECK_EQ(clickCount, 2);

        UiEvent spaceDown{};
        spaceDown.handle = dsbox::WindowHandle{ 1 };
        spaceDown.payload = KeyDownEvent{ dsbox::KeyCode::Space, dsbox::KeyModifier::None, false };
        tree.dispatchEvent(spaceDown);
        DSBOX_UI_CHECK_EQ(clickCount, 3);
    }
}

void runContainerTests()
{
    dsbox_ui_tests::runTest("ClickFiresOnClickOnlyOnMatchingPress", testClickFiresOnClickOnlyOnMatchingPress);
    dsbox_ui_tests::runTest("FocusableRequestsFocusOnPress", testFocusableRequestsFocusOnPress);
    dsbox_ui_tests::runTest("PressedColorOverridesBackgroundWhilePressed", testPressedColorOverridesBackgroundWhilePressed);
    dsbox_ui_tests::runTest("OpacityScalesFillAlpha", testOpacityScalesFillAlpha);
    dsbox_ui_tests::runTest("OpacityIsClampedToUnitRange", testOpacityIsClampedToUnitRange);
    dsbox_ui_tests::runTest("HoverColorAppliesOnPointerEnterAndClearsOnLeave", testHoverColorAppliesOnPointerEnterAndClearsOnLeave);
    dsbox_ui_tests::runTest("NonInteractiveWrapperDoesNotSwallowBubbledEvent", testNonInteractiveWrapperDoesNotSwallowBubbledEvent);
    dsbox_ui_tests::runTest("ContentSizedContainerFitsItsChildren", testContentSizedContainerFitsItsChildren);
    dsbox_ui_tests::runTest("IncrementalRelayoutSkipsUntouchedContainerSubtree", testIncrementalRelayoutSkipsUntouchedContainerSubtree);
    dsbox_ui_tests::runTest("IsFocusableReflectsSetFocusable", testIsFocusableReflectsSetFocusable);
    dsbox_ui_tests::runTest("EnterAndSpaceActivateFocusedClickable", testEnterAndSpaceActivateFocusedClickable);
}
