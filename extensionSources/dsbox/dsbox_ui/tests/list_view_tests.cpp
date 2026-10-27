#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/list_view.hpp>
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
        TextMetrics measureText(FontHandle, std::string_view) override { return { 8.0f, 4.0f, 12.0f, 0.0f }; }
        std::vector<ShapedGlyph> shapeText(FontHandle, std::string_view) override { return {}; }
        ImageHandle registerImage(std::uint32_t, std::uint32_t, const std::uint8_t*) override { return kInvalidImageHandle; }
        void submit(const DisplayList&) override {}
    };

    class TestActions final : public IUiElementActions
    {
    public:
        int markDirtyCount = 0;
        int requestFocusCount = 0;

        void markDirty(UiElementId, UiDirtyLevel = UiDirtyLevel::Layout) override { ++markDirtyCount; }
        void requestFocus(UiElementId) override { ++requestFocusCount; }
        void setPointerCapture(UiElementId) override {}
        void releasePointerCapture(UiElementId) override {}
    };

    UiEvent targetedEvent(const ListView& list, UiEventPayload payload)
    {
        UiEvent event{};
        event.target = list.id();
        event.phase = UiEventPhase::Target;
        event.payload = std::move(payload);
        return event;
    }

    std::vector<std::vector<std::string>> threeRows()
    {
        return {
            { "a1", "a2" },
            { "b1", "b2" },
            { "c1", "c2" },
        };
    }

    void testSetRowsClampsOutOfRangeSelection()
    {
        ListView list(1);
        list.setRows(threeRows());
        list.setSelectedIndex(2);
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(2));

        list.setRows({ { "x1" } }); // only one row now -- old index 2 is out of range
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(0));

        list.setRows({}); // empty -- selection clears entirely
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), ListView::kInvalidIndex);
    }

    void testSetSelectedIndexDoesNotFireCallback()
    {
        ListView list(1);
        list.setRows(threeRows());

        int callCount = 0;
        list.setOnSelectionChanged([&](std::size_t) { ++callCount; });

        list.setSelectedIndex(1);
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(1));
        DSBOX_UI_CHECK_EQ(callCount, 0);
    }

    void testPointerDownSelectsRowByPosition()
    {
        ListView list(1, UiElementIdMode::Unique, "list");
        list.setRows(threeRows());
        list.setRowHeight(20.0f);
        list.setHeaderHeight(10.0f);

        DummyRenderBackend backend;
        list.measure(UiConstraints{}, backend);
        list.arrange(UiRect{ 0.0f, 0.0f, 100.0f, 100.0f }, backend);

        TestActions actions;
        int selectedFromCallback = static_cast<int>(ListView::kInvalidIndex);
        list.setOnSelectionChanged([&](std::size_t index) { selectedFromCallback = static_cast<int>(index); });

        // headerHeight 10, rowHeight 20 -- y=35 lands in row index 1
        // (header [0,10), row0 [10,30), row1 [30,50)).
        list.handleEvent(targetedEvent(list, PointerDownEvent{ 5.0f, 35.0f, dsbox::MouseButton::Left }), actions);

        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(1));
        DSBOX_UI_CHECK_EQ(selectedFromCallback, 1);
        DSBOX_UI_CHECK_EQ(actions.requestFocusCount, 1);
        DSBOX_UI_CHECK(actions.markDirtyCount >= 1);
    }

    void testArrowKeysMoveSelection()
    {
        ListView list(1);
        list.setRows(threeRows());

        TestActions actions;
        list.handleEvent(targetedEvent(list, KeyDownEvent{ dsbox::KeyCode::Down, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(0)); // nothing selected yet -- Down selects the first row

        list.handleEvent(targetedEvent(list, KeyDownEvent{ dsbox::KeyCode::Down, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(1));

        list.handleEvent(targetedEvent(list, KeyDownEvent{ dsbox::KeyCode::Up, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK_EQ(list.selectedIndex(), static_cast<std::size_t>(0));
    }

    void testEnterActivatesSelectedRow()
    {
        ListView list(1);
        list.setRows(threeRows());
        list.setSelectedIndex(2);

        TestActions actions;
        int activated = -1;
        list.setOnRowActivated([&](std::size_t index) { activated = static_cast<int>(index); });

        list.handleEvent(targetedEvent(list, KeyDownEvent{ dsbox::KeyCode::Enter, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK_EQ(activated, 2);
    }
}

void runListViewTests()
{
    dsbox_ui_tests::runTest("SetRowsClampsOutOfRangeSelection", testSetRowsClampsOutOfRangeSelection);
    dsbox_ui_tests::runTest("SetSelectedIndexDoesNotFireCallback", testSetSelectedIndexDoesNotFireCallback);
    dsbox_ui_tests::runTest("PointerDownSelectsRowByPosition", testPointerDownSelectsRowByPosition);
    dsbox_ui_tests::runTest("ArrowKeysMoveSelection", testArrowKeysMoveSelection);
    dsbox_ui_tests::runTest("EnterActivatesSelectedRow", testEnterActivatesSelectedRow);
}
