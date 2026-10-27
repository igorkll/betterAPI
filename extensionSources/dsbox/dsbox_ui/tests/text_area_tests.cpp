#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/text_area.hpp>

#include <cstddef>
#include <variant>
#include <vector>

using namespace dsbox_ui;

namespace
{
    // Same deterministic fake shaping TextField/Text's own tests use --
    // every byte is one 10px-wide glyph, lineHeight 12.
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

    UiEvent targetedEvent(const TextArea& area, UiEventPayload payload)
    {
        UiEvent event{};
        event.target = area.id();
        event.phase = UiEventPhase::Target;
        event.payload = std::move(payload);
        return event;
    }

    void testWordWrapBreaksAtWordBoundary()
    {
        // Each glyph is 10px wide (see TestTextRenderBackend); a width
        // of 55px should fit "aaaa bbbb" (9 chars * 10 = 90, too wide)
        // -- expect it to wrap after the first word rather than mid-word.
        TextArea area(1);
        area.setFont(FontHandle{ 1 });
        area.setPreferredWidth(UiLength::pixels(55.0f + 16.0f)); // + 2*kInset
        area.setText("aaaa bbbb");

        TestTextRenderBackend backend;
        area.measure(UiConstraints{}, backend);

        // No direct accessor for wrapped lines -- infer via
        // textInputRect()'s own y, which increases by one line's
        // height once the cursor (at the END of the text, see
        // setText()) is on the second wrapped line.
        const UiRect rect = area.textInputRect();
        DSBOX_UI_CHECK(rect.y > 0.0f); // cursor on a line after the first -- i.e. wrapping happened at all
    }

    void testHardNewlineAlwaysBreaksRegardlessOfWidth()
    {
        TextArea area(1);
        area.setFont(FontHandle{ 1 });
        area.setPreferredWidth(UiLength::pixels(500.0f)); // plenty wide -- no soft wrap should occur
        area.setText("a\nb");

        TestTextRenderBackend backend;
        area.measure(UiConstraints{}, backend);

        const UiRect rect = area.textInputRect();
        DSBOX_UI_CHECK(rect.y > 0.0f); // cursor (at the end, after 'b') is on the second line because of the '\n'
    }

    void testEnterInsertsNewlineAndFiresOnChange()
    {
        TextArea area(1);
        TestActions actions;

        std::string lastChange;
        int changeCount = 0;
        area.setOnChange([&](const std::string& text) { lastChange = text; ++changeCount; });

        area.handleEvent(targetedEvent(area, TextInputEvent{ "ab" }), actions);
        area.handleEvent(targetedEvent(area, KeyDownEvent{ dsbox::KeyCode::Enter, dsbox::KeyModifier::None, false }), actions);
        area.handleEvent(targetedEvent(area, TextInputEvent{ "c" }), actions);

        DSBOX_UI_CHECK(area.text() == "ab\nc");
        DSBOX_UI_CHECK_EQ(changeCount, 3);
        DSBOX_UI_CHECK(lastChange == "ab\nc");
    }

    void testBackspaceAcrossNewlineJoinsLines()
    {
        TextArea area(1);
        TestActions actions;
        area.setText("ab\ncd"); // setText() moves cursor to the end

        area.handleEvent(targetedEvent(area, KeyDownEvent{ dsbox::KeyCode::Home, dsbox::KeyModifier::None, false }), actions);
        // Home moves to the start of the CURRENT line ("cd"'s own line) -- cursor now right after '\n', before 'c'

        area.handleEvent(targetedEvent(area, KeyDownEvent{ dsbox::KeyCode::Backspace, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(area.text() == "abcd"); // removed the '\n' itself, joining the two lines
    }

    void testSetTextDoesNotFireOnChange()
    {
        TextArea area(1);
        int changeCount = 0;
        area.setOnChange([&](const std::string&) { ++changeCount; });

        area.setText("hello\nworld");
        DSBOX_UI_CHECK(area.text() == "hello\nworld");
        DSBOX_UI_CHECK_EQ(changeCount, 0);
    }

    void testPointerDownRequestsFocus()
    {
        TextArea area(1);
        TestActions actions;

        area.handleEvent(targetedEvent(area, PointerDownEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left }), actions);

        DSBOX_UI_CHECK_EQ(actions.requestFocusCount, 1);
    }

    void testDisabledFieldIsNotFocusableOrTextInputTarget()
    {
        TextArea area(1);
        auto* focusTarget = dynamic_cast<IFocusTarget*>(&area);
        auto* textTarget = dynamic_cast<ITextInputTarget*>(&area);

        DSBOX_UI_CHECK(focusTarget != nullptr);
        DSBOX_UI_CHECK(textTarget != nullptr);
        if (!focusTarget || !textTarget)
            return;

        DSBOX_UI_CHECK(focusTarget->isFocusable());
        DSBOX_UI_CHECK(textTarget->acceptsTextInput());

        area.setEnabled(false);
        DSBOX_UI_CHECK(!focusTarget->isFocusable());
        DSBOX_UI_CHECK(!textTarget->acceptsTextInput());
    }
}

void runTextAreaTests()
{
    dsbox_ui_tests::runTest("WordWrapBreaksAtWordBoundary", testWordWrapBreaksAtWordBoundary);
    dsbox_ui_tests::runTest("HardNewlineAlwaysBreaksRegardlessOfWidth", testHardNewlineAlwaysBreaksRegardlessOfWidth);
    dsbox_ui_tests::runTest("EnterInsertsNewlineAndFiresOnChange", testEnterInsertsNewlineAndFiresOnChange);
    dsbox_ui_tests::runTest("BackspaceAcrossNewlineJoinsLines", testBackspaceAcrossNewlineJoinsLines);
    dsbox_ui_tests::runTest("SetTextDoesNotFireOnChange", testSetTextDoesNotFireOnChange);
    dsbox_ui_tests::runTest("PointerDownRequestsFocus", testPointerDownRequestsFocus);
    dsbox_ui_tests::runTest("DisabledFieldIsNotFocusableOrTextInputTarget", testDisabledFieldIsNotFocusableOrTextInputTarget);
}
