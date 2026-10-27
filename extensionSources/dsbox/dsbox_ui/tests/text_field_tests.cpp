#include "test_framework.hpp"

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/render_backend.hpp>
#include <dsbox_ui/text_field.hpp>

#include <cstddef>
#include <variant>
#include <vector>

using namespace dsbox_ui;

namespace
{
    // Same deterministic fake shaping text_tests.cpp's
    // TestTextRenderBackend uses -- every byte is one 10px-wide glyph.
    // Reused here rather than shared, since these two test binaries
    // compile independently and neither dsbox_ui itself nor its public
    // headers offer a test-only backend to share.
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

    // Minimal IUiElementActions double: just counts calls, so a test
    // can assert TextField actually marks itself dirty/requests focus
    // at the right moments without needing a real WindowUiTree.
    class TestActions final : public IUiElementActions
    {
    public:
        int markDirtyCount = 0;
        int requestFocusCount = 0;
        UiElementId lastFocusRequested = kInvalidUiElementId;

        void markDirty(UiElementId, UiDirtyLevel = UiDirtyLevel::Layout) override { ++markDirtyCount; }
        void requestFocus(UiElementId id) override { ++requestFocusCount; lastFocusRequested = id; }
        void setPointerCapture(UiElementId) override {}
        void releasePointerCapture(UiElementId) override {}
    };

    // TextField only ever reacts to an event targeted AT IT (see
    // handleEvent()'s own guard) -- every helper below builds exactly
    // that: phase Target, target == field.id().
    UiEvent targetedEvent(const TextField& field, UiEventPayload payload)
    {
        UiEvent event{};
        event.target = field.id();
        event.phase = UiEventPhase::Target;
        event.payload = std::move(payload);
        return event;
    }

    void testTypingInsertsAtCursorAndFiresOnChange()
    {
        TextField field(1);
        TestActions actions;

        std::string lastChange;
        int changeCount = 0;
        field.setOnChange([&](const std::string& text) { lastChange = text; ++changeCount; });

        field.handleEvent(targetedEvent(field, TextInputEvent{ "ab" }), actions);
        DSBOX_UI_CHECK(field.text() == "ab");
        DSBOX_UI_CHECK_EQ(changeCount, 1);
        DSBOX_UI_CHECK(lastChange == "ab");

        // Cursor is now after "ab" (byte offset 2) -- typing again
        // inserts AT the cursor, i.e. appends here, not at the start.
        field.handleEvent(targetedEvent(field, TextInputEvent{ "c" }), actions);
        DSBOX_UI_CHECK(field.text() == "abc");
        DSBOX_UI_CHECK_EQ(changeCount, 2);
    }

    void testEmptyTextInputEventDoesNothing()
    {
        TextField field(1);
        TestActions actions;
        field.setText("x");

        int changeCount = 0;
        field.setOnChange([&](const std::string&) { ++changeCount; });

        const bool handled = field.handleEvent(targetedEvent(field, TextInputEvent{ "" }), actions);

        DSBOX_UI_CHECK(!handled); // see handleEvent()'s own doc: empty text is a no-op, not "handled"
        DSBOX_UI_CHECK(field.text() == "x");
        DSBOX_UI_CHECK_EQ(changeCount, 0);
    }

    void testBackspaceAndDeleteRemoveOneByteAtCursor()
    {
        TextField field(1);
        TestActions actions;
        field.setText("abc"); // setText() moves the cursor to the end (byte 3)

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Left, dsbox::KeyModifier::None, false }), actions);
        // cursor now at byte 2, between 'b' and 'c'

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Backspace, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(field.text() == "ac"); // removed 'b', the byte BEFORE the cursor

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Delete, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(field.text() == "a"); // removed 'c', the byte AT the cursor

        // Backspace at the very start of an empty-after-this field is a no-op.
        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Home, dsbox::KeyModifier::None, false }), actions);
        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Backspace, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(field.text() == "a");
    }

    void testHomeAndEndMoveCursorToTextBoundaries()
    {
        // Cursor position isn't directly observable from the outside,
        // so this test infers it the same way a real caller would --
        // through what Backspace/Delete affect next.
        TextField field(1);
        TestActions actions;
        field.setText("abc");

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Home, dsbox::KeyModifier::None, false }), actions);
        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Delete, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(field.text() == "bc"); // Home put the cursor before 'a'

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::End, dsbox::KeyModifier::None, false }), actions);
        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Backspace, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(field.text() == "b"); // End put the cursor after 'c', Backspace removed it
    }

    void testEnterFiresOnSubmitButNotOnAutorepeat()
    {
        TextField field(1);
        TestActions actions;

        int submitCount = 0;
        field.setOnSubmit([&] { ++submitCount; });

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Enter, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK_EQ(submitCount, 1);

        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Enter, dsbox::KeyModifier::None, true }), actions);
        DSBOX_UI_CHECK_EQ(submitCount, 1); // autorepeat does not fire it again
    }

    void testSetTextMovesCursorToEndAndDoesNotFireOnChange()
    {
        TextField field(1);
        int changeCount = 0;
        field.setOnChange([&](const std::string&) { ++changeCount; });

        field.setText("hello");
        DSBOX_UI_CHECK(field.text() == "hello");
        DSBOX_UI_CHECK_EQ(changeCount, 0); // setText() is programmatic, not a user edit -- see its own doc

        TestActions actions;
        field.handleEvent(targetedEvent(field, KeyDownEvent{ dsbox::KeyCode::Backspace, dsbox::KeyModifier::None, false }), actions);
        DSBOX_UI_CHECK(field.text() == "hell"); // cursor was at the end, so Backspace removed the last char
    }

    void testPointerDownRequestsFocus()
    {
        TextField field(1);
        TestActions actions;

        field.handleEvent(targetedEvent(field, PointerDownEvent{ 5.0f, 5.0f, dsbox::MouseButton::Left }), actions);

        DSBOX_UI_CHECK_EQ(actions.requestFocusCount, 1);
        DSBOX_UI_CHECK(actions.lastFocusRequested == field.id());
    }

    void testFocusInAndOutSetFocusedState()
    {
        // m_focused isn't directly observable either -- inferred here
        // through paint(): the caret (an extra RectCommand beyond the
        // field's own border/background pair) only appears while
        // focused.
        TextField field(1);
        field.setFont(FontHandle{ 1 });
        field.setText("x");

        TestTextRenderBackend backend;
        UiSize size = field.measure(UiConstraints{}, backend);
        field.arrange(UiRect{ 0.0f, 0.0f, size.width, size.height }, backend);

        TestActions actions;

        DisplayListBuilder unfocused;
        field.paint(unfocused);
        const std::size_t unfocusedCommandCount = unfocused.take().commands.size();

        field.handleEvent(targetedEvent(field, FocusInEvent{}), actions);

        DisplayListBuilder focused;
        field.paint(focused);
        const std::size_t focusedCommandCount = focused.take().commands.size();

        DSBOX_UI_CHECK_EQ(focusedCommandCount, unfocusedCommandCount + 1); // exactly one extra command: the caret

        field.handleEvent(targetedEvent(field, FocusOutEvent{}), actions);

        DisplayListBuilder blurred;
        field.paint(blurred);
        DSBOX_UI_CHECK_EQ(blurred.take().commands.size(), unfocusedCommandCount);
    }

    void testDisabledFieldIsNotFocusableOrTextInputTarget()
    {
        TextField field(1);
        auto* focusTarget = dynamic_cast<IFocusTarget*>(&field);
        auto* textTarget = dynamic_cast<ITextInputTarget*>(&field);

        DSBOX_UI_CHECK(focusTarget != nullptr);
        DSBOX_UI_CHECK(textTarget != nullptr);
        if (!focusTarget || !textTarget)
            return;

        DSBOX_UI_CHECK(focusTarget->isFocusable());
        DSBOX_UI_CHECK(textTarget->acceptsTextInput());

        field.setEnabled(false);
        DSBOX_UI_CHECK(!focusTarget->isFocusable());
        DSBOX_UI_CHECK(!textTarget->acceptsTextInput());
    }

    void testMeasureClampsToPreferredSizeConstraints()
    {
        TextField field(1);
        field.setPreferredWidth(UiLength::pixels(150.0f));
        field.setPreferredHeight(UiLength::pixels(30.0f));

        TestTextRenderBackend backend;
        UiSize size = field.measure(UiConstraints{}, backend); // unbounded

        DSBOX_UI_CHECK_EQ(size.width, 150.0f);
        DSBOX_UI_CHECK_EQ(size.height, 30.0f);
    }
}

void runTextFieldTests()
{
    dsbox_ui_tests::runTest("TypingInsertsAtCursorAndFiresOnChange", testTypingInsertsAtCursorAndFiresOnChange);
    dsbox_ui_tests::runTest("EmptyTextInputEventDoesNothing", testEmptyTextInputEventDoesNothing);
    dsbox_ui_tests::runTest("BackspaceAndDeleteRemoveOneByteAtCursor", testBackspaceAndDeleteRemoveOneByteAtCursor);
    dsbox_ui_tests::runTest("HomeAndEndMoveCursorToTextBoundaries", testHomeAndEndMoveCursorToTextBoundaries);
    dsbox_ui_tests::runTest("EnterFiresOnSubmitButNotOnAutorepeat", testEnterFiresOnSubmitButNotOnAutorepeat);
    dsbox_ui_tests::runTest("SetTextMovesCursorToEndAndDoesNotFireOnChange", testSetTextMovesCursorToEndAndDoesNotFireOnChange);
    dsbox_ui_tests::runTest("PointerDownRequestsFocus", testPointerDownRequestsFocus);
    dsbox_ui_tests::runTest("FocusInAndOutSetFocusedState", testFocusInAndOutSetFocusedState);
    dsbox_ui_tests::runTest("DisabledFieldIsNotFocusableOrTextInputTarget", testDisabledFieldIsNotFocusableOrTextInputTarget);
    dsbox_ui_tests::runTest("MeasureClampsToPreferredSizeConstraints", testMeasureClampsToPreferredSizeConstraints);
}
