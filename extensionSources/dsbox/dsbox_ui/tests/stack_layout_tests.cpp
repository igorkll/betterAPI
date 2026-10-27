#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/stack_layout.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <vector>

using namespace dsbox_ui;

namespace
{
    // Minimal IUiElement (+ optional IFlexItem) double for exercising
    // StackLayout in isolation, without needing a real Container.
    // measure() reports whatever intrinsic width/height were
    // configured, clamped into the incoming constraints -- close
    // enough to Container's own Pixel-length behavior (see its
    // resolveDimension()) for these tests, without dragging in every
    // other Container concern.
    class TestFlexElement final : public IUiElement, public IFlexItem
    {
    public:
        explicit TestFlexElement(UiElementId id) : m_id(id) {}

        void setIntrinsicSize(float width, float height)
        {
            m_intrinsicWidth = width;
            m_intrinsicHeight = height;
        }

        // No-op (flexGrow() stays 0 for every axis) unless called.
        void setFlex(UiAxis axis, float weight)
        {
            m_flexAxis = axis;
            m_flexWeight = weight;
        }

        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return UiElementIdMode::Instance; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend&) override
        {
            ++measureCount;
            lastConstraints = constraints;
            return {
                std::clamp(m_intrinsicWidth, constraints.minWidth, constraints.maxWidth),
                std::clamp(m_intrinsicHeight, constraints.minHeight, constraints.maxHeight)
            };
        }

        void arrange(const UiRect& rect, IUiRenderBackend&) override { m_bounds = rect; }
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder&) const override {}
        bool handleEvent(const UiEvent&, IUiElementActions&) override { return false; }
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        float flexGrow(UiAxis axis) const noexcept override
        {
            return (axis == m_flexAxis) ? m_flexWeight : 0.0f;
        }

        int measureCount = 0;
        UiConstraints lastConstraints{};

    private:
        UiElementId m_id;
        float m_intrinsicWidth = 0.0f;
        float m_intrinsicHeight = 0.0f;
        UiAxis m_flexAxis = UiAxis::Horizontal;
        float m_flexWeight = 0.0f;
        UiRect m_bounds;
    };

    // measureText()/loadFont() etc. are never exercised by these tests
    // -- this backend exists only so StackLayout::layout() has
    // something to pass through to each child's measure().
    class DummyRenderBackend final : public IUiRenderBackend
    {
    public:
        FontHandle loadFont(std::string_view, float) override { return kInvalidFontHandle; }
        TextMetrics measureText(FontHandle, std::string_view) override { return {}; }
        std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return {}; }
        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    void testNoFlexChildrenBehavesAsBefore()
    {
        // Three fixed-size children, vertical stack, no gap -- pure
        // regression guard: StackLayout with nothing flexible in it
        // should place children exactly as it always did.
        std::vector<std::unique_ptr<IUiElement>> children;
        std::vector<TestFlexElement*> raw;

        for (UiElementId id = 1; id <= 3; ++id)
        {
            auto child = std::make_unique<TestFlexElement>(id);
            child->setIntrinsicSize(999.0f, 20.0f); // width irrelevant on a vertical stack's main axis
            raw.push_back(child.get());
            children.push_back(std::move(child));
        }

        StackLayout layout(StackDirection::Vertical, 0.0f);
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        DSBOX_UI_CHECK(raw[0]->bounds() == (UiRect{ 0.0f, 0.0f, 100.0f, 20.0f }));
        DSBOX_UI_CHECK(raw[1]->bounds() == (UiRect{ 0.0f, 20.0f, 100.0f, 20.0f }));
        DSBOX_UI_CHECK(raw[2]->bounds() == (UiRect{ 0.0f, 40.0f, 100.0f, 20.0f }));
    }

    void testSingleFlexChildTakesAllRemainingSpace()
    {
        std::vector<std::unique_ptr<IUiElement>> children;

        auto fixed = std::make_unique<TestFlexElement>(1);
        fixed->setIntrinsicSize(999.0f, 30.0f);
        TestFlexElement* fixedPtr = fixed.get();
        children.push_back(std::move(fixed));

        auto flex = std::make_unique<TestFlexElement>(2);
        flex->setFlex(UiAxis::Vertical, 1.0f);
        TestFlexElement* flexPtr = flex.get();
        children.push_back(std::move(flex));

        StackLayout layout(StackDirection::Vertical, 0.0f);
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        DSBOX_UI_CHECK(fixedPtr->bounds() == (UiRect{ 0.0f, 0.0f, 100.0f, 30.0f }));
        // 100 total - 30 taken by the fixed sibling - 0 gap = 70 left,
        // and there's only one flexible child, so it gets all of it.
        DSBOX_UI_CHECK(flexPtr->bounds() == (UiRect{ 0.0f, 30.0f, 100.0f, 70.0f }));
    }

    void testMultipleFlexChildrenSplitProportionally()
    {
        std::vector<std::unique_ptr<IUiElement>> children;

        auto a = std::make_unique<TestFlexElement>(1);
        a->setFlex(UiAxis::Vertical, 1.0f);
        TestFlexElement* aPtr = a.get();
        children.push_back(std::move(a));

        auto b = std::make_unique<TestFlexElement>(2);
        b->setFlex(UiAxis::Vertical, 3.0f);
        TestFlexElement* bPtr = b.get();
        children.push_back(std::move(b));

        StackLayout layout(StackDirection::Vertical, 0.0f);
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 90.0f }, backend);

        // No non-flexible sibling consumes anything, so the full 90px
        // is free space, split 1:3 between the two flexible children.
        DSBOX_UI_CHECK(aPtr->bounds() == (UiRect{ 0.0f, 0.0f, 100.0f, 22.5f }));
        DSBOX_UI_CHECK(bPtr->bounds() == (UiRect{ 0.0f, 22.5f, 100.0f, 67.5f }));
    }

    void testMixedFixedAndFlexChildrenWithGap()
    {
        std::vector<std::unique_ptr<IUiElement>> children;

        auto fixed = std::make_unique<TestFlexElement>(1);
        fixed->setIntrinsicSize(999.0f, 20.0f);
        TestFlexElement* fixedPtr = fixed.get();
        children.push_back(std::move(fixed));

        auto flexA = std::make_unique<TestFlexElement>(2);
        flexA->setFlex(UiAxis::Vertical, 1.0f);
        TestFlexElement* flexAPtr = flexA.get();
        children.push_back(std::move(flexA));

        auto flexB = std::make_unique<TestFlexElement>(3);
        flexB->setFlex(UiAxis::Vertical, 1.0f);
        TestFlexElement* flexBPtr = flexB.get();
        children.push_back(std::move(flexB));

        StackLayout layout(StackDirection::Vertical, 10.0f);
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        // gapTotal = 2 gaps * 10 = 20; mainAvailable = 80.
        // Fixed child takes 20 of that, leaving 60 free, split evenly
        // (weight 1:1) between the two flexible children -> 30 each.
        DSBOX_UI_CHECK(fixedPtr->bounds() == (UiRect{ 0.0f, 0.0f, 100.0f, 20.0f }));
        DSBOX_UI_CHECK(flexAPtr->bounds() == (UiRect{ 0.0f, 30.0f, 100.0f, 30.0f })); // 20 + 10 gap
        DSBOX_UI_CHECK(flexBPtr->bounds() == (UiRect{ 0.0f, 70.0f, 100.0f, 30.0f })); // 60 + 10 gap

        // The last child's bottom edge should land exactly on the
        // content rect's own bottom edge -- every pixel of the 100px
        // extent accounted for (20 + 10 + 30 + 10 + 30 == 100).
        DSBOX_UI_CHECK_EQ(flexBPtr->bounds().bottom(), 100.0f);
    }

    void testFlexOnWrongAxisIsNotFlexible()
    {
        // Configured flexible on the HORIZONTAL axis, but placed in a
        // VERTICAL stack (main axis is Vertical) -- flexGrowFor()
        // should report 0 for this child here, so it falls back to
        // being measured from its own intrinsic size like any other
        // non-flexible child, not treated as eligible for free-space
        // distribution just because it implements IFlexItem at all.
        std::vector<std::unique_ptr<IUiElement>> children;

        auto child = std::make_unique<TestFlexElement>(1);
        child->setIntrinsicSize(999.0f, 40.0f);
        child->setFlex(UiAxis::Horizontal, 5.0f); // wrong axis for this stack
        TestFlexElement* childPtr = child.get();
        children.push_back(std::move(child));

        StackLayout layout(StackDirection::Vertical, 0.0f);
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        DSBOX_UI_CHECK(childPtr->bounds() == (UiRect{ 0.0f, 0.0f, 100.0f, 40.0f }));
    }

    void testCrossAlignCenterPositionsChildAtItsOwnMeasuredSize()
    {
        // Horizontal stack: cross axis is vertical. A 40x20 child in a
        // 100-wide x 100-tall content rect should, under
        // CrossAlign::Center, keep its own 20px height (not stretch to
        // 100) and sit vertically centered -- crossOffset = (100-20)/2
        // = 40.
        std::vector<std::unique_ptr<IUiElement>> children;

        auto child = std::make_unique<TestFlexElement>(1);
        child->setIntrinsicSize(40.0f, 20.0f);
        TestFlexElement* childPtr = child.get();
        children.push_back(std::move(child));

        StackLayout layout(StackDirection::Horizontal, 0.0f, CrossAlign::Center);
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        DSBOX_UI_CHECK(childPtr->bounds() == (UiRect{ 0.0f, 40.0f, 40.0f, 20.0f }));
    }

    void testMeasureContentSumsMainAxisAndMaxesCrossAxis()
    {
        // Vertical stack, three children with distinct intrinsic sizes
        // and a non-zero gap -- measureContent() should report the SUM
        // of main-axis (height) sizes plus gaps, and the MAX of
        // cross-axis (width) sizes, matching what layout()'s own Pass 1
        // would compute for the same non-flexible children (see this
        // method's own declaration, stack_layout.hpp).
        std::vector<std::unique_ptr<IUiElement>> children;

        auto a = std::make_unique<TestFlexElement>(1);
        a->setIntrinsicSize(30.0f, 10.0f);
        children.push_back(std::move(a));

        auto b = std::make_unique<TestFlexElement>(2);
        b->setIntrinsicSize(50.0f, 20.0f); // widest -- decides the cross-axis answer
        children.push_back(std::move(b));

        auto c = std::make_unique<TestFlexElement>(3);
        c->setIntrinsicSize(10.0f, 5.0f);
        children.push_back(std::move(c));

        StackLayout layout(StackDirection::Vertical, 4.0f); // 2 gaps * 4 = 8
        DummyRenderBackend backend;

        UiConstraints constraints; // unbounded -- see this test's own "natural size" framing
        const UiSize content = layout.measureContent(children, constraints, backend);

        DSBOX_UI_CHECK_EQ(content.width, 50.0f); // widest child
        DSBOX_UI_CHECK_EQ(content.height, 10.0f + 20.0f + 5.0f + 8.0f); // sum + gaps = 43
    }

    void testMeasureContentClampsToOuterConstraints()
    {
        // A content-fit answer still can't exceed whatever bound its
        // OWN parent imposed -- see measureContent()'s own doc
        // (element.hpp) for why `constraints` is an outer clamp, not
        // something children are stretched to fill.
        std::vector<std::unique_ptr<IUiElement>> children;

        auto a = std::make_unique<TestFlexElement>(1);
        a->setIntrinsicSize(500.0f, 500.0f);
        children.push_back(std::move(a));

        StackLayout layout(StackDirection::Vertical, 0.0f);
        DummyRenderBackend backend;

        UiConstraints constraints;
        constraints.maxWidth = 100.0f;
        constraints.maxHeight = 50.0f;

        const UiSize content = layout.measureContent(children, constraints, backend);

        DSBOX_UI_CHECK_EQ(content.width, 100.0f);
        DSBOX_UI_CHECK_EQ(content.height, 50.0f);
    }

    void testCrossAlignStretchIsStillTheDefault()
    {
        // Regression guard: constructing a StackLayout without
        // explicitly naming a CrossAlign must behave exactly as before
        // CrossAlign existed -- full crossExtent, offset 0, regardless
        // of the child's own (smaller) intrinsic cross size.
        std::vector<std::unique_ptr<IUiElement>> children;

        auto child = std::make_unique<TestFlexElement>(1);
        child->setIntrinsicSize(40.0f, 20.0f);
        TestFlexElement* childPtr = child.get();
        children.push_back(std::move(child));

        StackLayout layout(StackDirection::Horizontal, 0.0f); // no CrossAlign named -- defaults to Stretch
        DummyRenderBackend backend;
        layout.layout(children, UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        DSBOX_UI_CHECK(childPtr->bounds() == (UiRect{ 0.0f, 0.0f, 40.0f, 100.0f }));
    }
}

void runStackLayoutTests()
{
    dsbox_ui_tests::runTest("NoFlexChildrenBehavesAsBefore", testNoFlexChildrenBehavesAsBefore);
    dsbox_ui_tests::runTest("SingleFlexChildTakesAllRemainingSpace", testSingleFlexChildTakesAllRemainingSpace);
    dsbox_ui_tests::runTest("MultipleFlexChildrenSplitProportionally", testMultipleFlexChildrenSplitProportionally);
    dsbox_ui_tests::runTest("MixedFixedAndFlexChildrenWithGap", testMixedFixedAndFlexChildrenWithGap);
    dsbox_ui_tests::runTest("FlexOnWrongAxisIsNotFlexible", testFlexOnWrongAxisIsNotFlexible);
    dsbox_ui_tests::runTest("CrossAlignCenterPositionsChildAtItsOwnMeasuredSize", testCrossAlignCenterPositionsChildAtItsOwnMeasuredSize);
    dsbox_ui_tests::runTest("MeasureContentSumsMainAxisAndMaxesCrossAxis", testMeasureContentSumsMainAxisAndMaxesCrossAxis);
    dsbox_ui_tests::runTest("MeasureContentClampsToOuterConstraints", testMeasureContentClampsToOuterConstraints);
    dsbox_ui_tests::runTest("CrossAlignStretchIsStillTheDefault", testCrossAlignStretchIsStillTheDefault);
}
