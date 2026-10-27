#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/scroll_view.hpp>

#include <cmath>
#include <memory>
#include <span>
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

    class TestActions final : public IUiElementActions
    {
    public:
        void markDirty(UiElementId, UiDirtyLevel = UiDirtyLevel::Layout) override { ++markDirtyCount; }
        void requestFocus(UiElementId) override { ++requestFocusCount; }
        void setPointerCapture(UiElementId) override {}
        void releasePointerCapture(UiElementId) override {}

        int markDirtyCount = 0;
        int requestFocusCount = 0;
    };

    // Fixed-size element -- reports exactly (intrinsicWidth,
    // intrinsicHeight) regardless of incoming constraints, clamped only
    // if the constraints are actually tighter -- enough to exercise
    // ScrollView's own "measure the child unbounded on the scrollable
    // axis" behavior (scroll_view.hpp) without needing a real Container.
    class FixedSizeElement final : public IUiElement
    {
    public:
        FixedSizeElement(UiElementId id, float width, float height)
            : m_id(id), m_width(width), m_height(height)
        {
        }

        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return UiElementIdMode::Instance; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend&) override
        {
            lastConstraints = constraints;
            return { constraints.clampWidth(m_width), constraints.clampHeight(m_height) };
        }

        void arrange(const UiRect& rect, IUiRenderBackend&) override { m_bounds = rect; }
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder&) const override {}
        bool handleEvent(const UiEvent&, IUiElementActions&) override { return false; }
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        UiConstraints lastConstraints{};

    private:
        UiElementId m_id;
        float m_width, m_height;
        UiRect m_bounds;
    };

    UiEvent targetedEvent(const ScrollView& view, UiEventPayload payload)
    {
        UiEvent event{};
        event.target = view.id();
        event.phase = UiEventPhase::Target;
        event.payload = std::move(payload);
        return event;
    }

    void testChildMeasuredUnboundedOnScrollableAxis()
    {
        ScrollView view(1); // vertical scroll on, horizontal off (defaults)
        view.setPreferredWidth(UiLength::pixels(100.0f));
        view.setPreferredHeight(UiLength::pixels(50.0f));

        auto child = std::make_unique<FixedSizeElement>(2, 9999.0f, 9999.0f);
        FixedSizeElement* childPtr = child.get();
        view.setChild(std::move(child));

        DummyRenderBackend backend;
        view.measure(UiConstraints{}, backend);

        // Vertical axis unbounded -- the child's own 9999 height passes
        // through untouched; horizontal is NOT scrollable, so it's
        // clamped to the viewport's own 100px width.
        DSBOX_UI_CHECK_EQ(childPtr->lastConstraints.maxWidth, 100.0f);
        DSBOX_UI_CHECK(std::isinf(childPtr->lastConstraints.maxHeight));
    }

    void testArrangeClampsScrollToContentExtent()
    {
        ScrollView view(1);
        view.setPreferredWidth(UiLength::pixels(100.0f));
        view.setPreferredHeight(UiLength::pixels(50.0f));
        view.setChild(std::make_unique<FixedSizeElement>(2, 80.0f, 200.0f));

        DummyRenderBackend backend;
        view.measure(UiConstraints{}, backend);
        view.arrange(UiRect{ 0.0f, 0.0f, 100.0f, 50.0f }, backend);

        DSBOX_UI_CHECK_EQ(view.maxScrollY(), 150.0f); // 200 content - 50 viewport
        DSBOX_UI_CHECK_EQ(view.maxScrollX(), 0.0f);    // horizontal scroll disabled by default

        view.setScrollY(1000.0f); // way past the end -- clamped
        DSBOX_UI_CHECK_EQ(view.scrollY(), 150.0f);

        view.setScrollY(-50.0f); // negative -- clamped to 0
        DSBOX_UI_CHECK_EQ(view.scrollY(), 0.0f);
    }

    void testWheelScrollsVerticalAxis()
    {
        ScrollView view(1);
        view.setChild(std::make_unique<FixedSizeElement>(2, 50.0f, 500.0f));

        DummyRenderBackend backend;
        view.measure(UiConstraints{}, backend);
        view.arrange(UiRect{ 0.0f, 0.0f, 280.0f, 200.0f }, backend);

        TestActions actions;
        const bool handled = view.handleEvent(targetedEvent(view, PointerWheelEvent{ 0.0f, -3.0f }), actions);

        DSBOX_UI_CHECK(handled);
        DSBOX_UI_CHECK(view.scrollY() > 0.0f); // negative scrollY (wheel "down") increases scroll offset
        DSBOX_UI_CHECK_EQ(actions.markDirtyCount, 1);
    }

    void testKeyScrollRespectsDisabledAxis()
    {
        ScrollView view(1); // vertical only
        view.setChild(std::make_unique<FixedSizeElement>(2, 500.0f, 500.0f));

        DummyRenderBackend backend;
        view.measure(UiConstraints{}, backend);
        view.arrange(UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        TestActions actions;
        const bool handledLeft = view.handleEvent(
            targetedEvent(view, KeyDownEvent{ dsbox::KeyCode::Left, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(!handledLeft); // horizontal disabled -- Left is not consumed
        DSBOX_UI_CHECK_EQ(view.scrollX(), 0.0f);

        const bool handledDown = view.handleEvent(
            targetedEvent(view, KeyDownEvent{ dsbox::KeyCode::Down, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(handledDown);
        DSBOX_UI_CHECK(view.scrollY() > 0.0f);
    }
}

void runScrollViewTests()
{
    dsbox_ui_tests::runTest("ChildMeasuredUnboundedOnScrollableAxis", testChildMeasuredUnboundedOnScrollableAxis);
    dsbox_ui_tests::runTest("ArrangeClampsScrollToContentExtent", testArrangeClampsScrollToContentExtent);
    dsbox_ui_tests::runTest("WheelScrollsVerticalAxis", testWheelScrollsVerticalAxis);
    dsbox_ui_tests::runTest("KeyScrollRespectsDisabledAxis", testKeyScrollRespectsDisabledAxis);
}
