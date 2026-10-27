#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/dropdown.hpp>
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
        void markDirty(UiElementId, UiDirtyLevel = UiDirtyLevel::Layout) override { ++markDirtyCount; }
        void requestFocus(UiElementId) override { ++requestFocusCount; }
        void setPointerCapture(UiElementId) override {}
        void releasePointerCapture(UiElementId) override {}

        int markDirtyCount = 0;
        int requestFocusCount = 0;
    };

    UiEvent targetedEvent(const Dropdown& dropdown, UiEventPayload payload)
    {
        UiEvent event{};
        event.target = dropdown.id();
        event.phase = UiEventPhase::Target;
        event.payload = std::move(payload);
        return event;
    }

    std::vector<std::string> threeItems()
    {
        return { "Alpha", "Beta", "Gamma" };
    }

    void testComboBoxStartsClosed()
    {
        Dropdown dropdown(1);
        dropdown.setMode(DropdownMode::ComboBox);
        DSBOX_UI_CHECK(!dropdown.isOpen());
    }

    void testListBoxIsAlwaysOpenAndIgnoresSetOpen()
    {
        Dropdown dropdown(1);
        dropdown.setMode(DropdownMode::ListBox);
        DSBOX_UI_CHECK(dropdown.isOpen());

        dropdown.setOpen(false); // no-op in ListBox mode -- see setOpen()'s own doc
        DSBOX_UI_CHECK(dropdown.isOpen());
    }

    void testClickOnHeaderTogglesOpen()
    {
        Dropdown dropdown(1);
        dropdown.setItems(threeItems());
        dropdown.setHeaderHeight(30.0f);

        DummyRenderBackend backend;
        dropdown.measure(UiConstraints{}, backend);
        dropdown.arrange(UiRect{ 0.0f, 0.0f, 200.0f, 200.0f }, backend);

        TestActions actions;
        dropdown.handleEvent(targetedEvent(dropdown, PointerDownEvent{ 5.0f, 10.0f, dsbox::MouseButton::Left }), actions);
        DSBOX_UI_CHECK(dropdown.isOpen());

        dropdown.handleEvent(targetedEvent(dropdown, PointerDownEvent{ 5.0f, 10.0f, dsbox::MouseButton::Left }), actions);
        DSBOX_UI_CHECK(!dropdown.isOpen());
    }

    void testClickOnItemSelectsAndClosesComboBox()
    {
        Dropdown dropdown(1);
        dropdown.setItems(threeItems());
        dropdown.setHeaderHeight(30.0f);
        dropdown.setItemHeight(20.0f);
        dropdown.setOpen(true);

        DummyRenderBackend backend;
        dropdown.measure(UiConstraints{}, backend);
        dropdown.arrange(UiRect{ 0.0f, 0.0f, 200.0f, 200.0f }, backend);

        TestActions actions;
        int selected = -1;
        dropdown.setOnSelectionChanged([&](std::size_t index) { selected = static_cast<int>(index); });

        // headerHeight 30, itemHeight 20 -- y=55 is in item row 1 ("Beta")
        dropdown.handleEvent(targetedEvent(dropdown, PointerDownEvent{ 5.0f, 55.0f, dsbox::MouseButton::Left }), actions);

        DSBOX_UI_CHECK_EQ(selected, 1);
        DSBOX_UI_CHECK_EQ(dropdown.selectedIndex(), static_cast<std::size_t>(1));
        DSBOX_UI_CHECK(!dropdown.isOpen()); // selecting an item closes a ComboBox
    }

    void testSetSelectedIndexDoesNotFireCallback()
    {
        Dropdown dropdown(1);
        dropdown.setItems(threeItems());

        int callCount = 0;
        dropdown.setOnSelectionChanged([&](std::size_t) { ++callCount; });

        dropdown.setSelectedIndex(2);
        DSBOX_UI_CHECK_EQ(dropdown.selectedIndex(), static_cast<std::size_t>(2));
        DSBOX_UI_CHECK_EQ(callCount, 0);
    }

    void testEscapeClosesOpenComboBox()
    {
        Dropdown dropdown(1);
        dropdown.setItems(threeItems());
        dropdown.setOpen(true);

        TestActions actions;
        dropdown.handleEvent(targetedEvent(dropdown, KeyDownEvent{ dsbox::KeyCode::Escape, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(!dropdown.isOpen());
    }

    void testFocusOutAutoCollapsesComboBox()
    {
        Dropdown dropdown(1);
        dropdown.setItems(threeItems());
        dropdown.setOpen(true);

        TestActions actions;
        dropdown.handleEvent(targetedEvent(dropdown, FocusOutEvent{}), actions);
        DSBOX_UI_CHECK(!dropdown.isOpen());
    }

    void testArrowKeysMoveSelectionWithoutOpening()
    {
        Dropdown dropdown(1);
        dropdown.setItems(threeItems());
        DSBOX_UI_CHECK(!dropdown.isOpen());

        TestActions actions;
        dropdown.handleEvent(targetedEvent(dropdown, KeyDownEvent{ dsbox::KeyCode::Down, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK_EQ(dropdown.selectedIndex(), static_cast<std::size_t>(0));
        DSBOX_UI_CHECK(!dropdown.isOpen()); // arrow-key navigation alone does not open it
    }
}

void runDropdownTests()
{
    dsbox_ui_tests::runTest("ComboBoxStartsClosed", testComboBoxStartsClosed);
    dsbox_ui_tests::runTest("ListBoxIsAlwaysOpenAndIgnoresSetOpen", testListBoxIsAlwaysOpenAndIgnoresSetOpen);
    dsbox_ui_tests::runTest("ClickOnHeaderTogglesOpen", testClickOnHeaderTogglesOpen);
    dsbox_ui_tests::runTest("ClickOnItemSelectsAndClosesComboBox", testClickOnItemSelectsAndClosesComboBox);
    dsbox_ui_tests::runTest("SetSelectedIndexDoesNotFireCallback", testSetSelectedIndexDoesNotFireCallback);
    dsbox_ui_tests::runTest("EscapeClosesOpenComboBox", testEscapeClosesOpenComboBox);
    dsbox_ui_tests::runTest("FocusOutAutoCollapsesComboBox", testFocusOutAutoCollapsesComboBox);
    dsbox_ui_tests::runTest("ArrowKeysMoveSelectionWithoutOpening", testArrowKeysMoveSelectionWithoutOpening);
}
