#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/length.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace dsbox_ui
{
    // Single-line editable text field: a focusable, clickable
    // background box with a text caret, driven by exactly the same
    // event vocabulary a Container-based button is (PointerDown/Up,
    // KeyDown, FocusIn/Out -- see Container's own "-- Interactivity --"
    // section, container.hpp) plus TextInputEvent/TextEditingEvent
    // (event.hpp), routed here only while this element holds focus AND
    // acceptsTextInput() is true -- see ITextInputTarget (element.hpp)
    // and WindowUiTree::setFocus()'s own IME start/stop wiring.
    //
    // v1 scope (deliberately minimal, matching this codebase's own
    // "v1" spirit throughout):
    //  - ASCII only. Cursor arithmetic below is a UTF-8 BYTE offset
    //    into text(), not a codepoint or grapheme index -- fine for
    //    ASCII (1 byte == 1 character == 1 glyph, matching
    //    VulkanUiRenderer's own ASCII-only glyph atlas -- see its
    //    class comment's "Text scope"), but a multi-byte UTF-8
    //    character typed via IME/TextInputEvent is accepted into
    //    text() verbatim yet can be split by Left/Right/Backspace/
    //    Delete, and will misposition the caret. A real UTF-8-aware
    //    (or grapheme-aware) caret is a follow-up, not a v1 concern.
    //  - No selection (click-drag, Shift+arrows) -- a single caret
    //    position is all there is.
    //  - No horizontal scrolling: text longer than the field's own
    //    width simply overflows past its right edge, clipped to the
    //    field's own bounds (see paint()) but never scrolled back into
    //    view as the caret moves past it.
    //  - TextEditingEvent (IME composition preview) is accepted (a
    //    real IME does not visibly break against this field) but has
    //    no visual effect here -- only committed TextInputEvent text
    //    actually appears.
    //  - No caret blink -- solid while focused, invisible otherwise.
    class DSBOX_UI_API TextField final : public IUiElement, public IFocusTarget, public ITextInputTarget
    {
    public:
        explicit TextField(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~TextField() override;

        TextField(const TextField&) = delete;
        TextField& operator=(const TextField&) = delete;

        // -- IUiElement --
        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_idMode; }
        std::string_view name() const noexcept override { return m_name; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) override;
        void arrange(const UiRect& rect, IUiRenderBackend& backend) override;
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder& out) const override;
        bool handleEvent(const UiEvent& event, IUiElementActions& actions) override;
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        // -- IFocusTarget --
        bool isFocusable() const noexcept override { return m_enabled; }

        // -- ITextInputTarget --
        bool acceptsTextInput() const noexcept override { return m_enabled; }
        UiRect textInputRect() const noexcept override;

        // -- Configuration --

        void setFont(FontHandle font) { m_font = font; }

        // Replaces the whole value and moves the caret to its end.
        // Does NOT fire onChange() -- that only fires for edits made
        // BY THE USER (typing/Backspace/Delete), the same distinction
        // a real <input>'s "input" event makes against setting
        // .value from script.
        void setText(std::string text);
        const std::string& text() const noexcept { return m_text; }

        // Shown (in placeholderColor) whenever text() is empty.
        void setPlaceholder(std::string placeholder) { m_placeholder = std::move(placeholder); }

        void setTextColor(UiColor color) { m_textColor = color; }
        void setPlaceholderColor(UiColor color) { m_placeholderColor = color; }
        void setBackgroundColor(UiColor color) { m_backgroundColor = color; }
        void setBorderColor(UiColor color) { m_borderColor = color; }
        void setFocusedBorderColor(UiColor color) { m_focusedBorderColor = color; }
        void setCornerRadius(float radius) { m_cornerRadius = radius; }

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

        // False disables both focusability and text input (see
        // isFocusable()/acceptsTextInput() above). A disabled field
        // already holding focus is NOT forcibly blurred by this call
        // alone -- WindowUiTree only re-checks focusability/text-input
        // eligibility on the NEXT focus change.
        void setEnabled(bool enabled) { m_enabled = enabled; }

        // Fires after text() actually changes as a direct result of a
        // TextInputEvent, Backspace, or Delete this element handled --
        // never for setText().
        void setOnChange(std::function<void(const std::string&)> onChange) { m_onChange = std::move(onChange); }

        // Fires on Enter/KeypadEnter (KeyDownEvent, not OS autorepeat)
        // while focused.
        void setOnSubmit(std::function<void()> onSubmit) { m_onSubmit = std::move(onSubmit); }

    private:
        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        std::string m_text;
        std::string m_placeholder;
        std::size_t m_cursor = 0; // UTF-8 byte offset into m_text -- see this class's own "v1 scope"

        FontHandle m_font = kInvalidFontHandle;
        UiColor m_textColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        UiColor m_placeholderColor{ 0.55f, 0.55f, 0.55f, 1.0f };
        UiColor m_backgroundColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        UiColor m_borderColor{ 0.7f, 0.7f, 0.7f, 1.0f };
        UiColor m_focusedBorderColor{ 0.25f, 0.45f, 0.85f, 1.0f };
        float m_cornerRadius = 4.0f;

        UiLength m_preferredWidth = UiLength::pixels(200.0f);
        UiLength m_preferredHeight = UiLength::pixels(36.0f);

        bool m_enabled = true;
        bool m_focused = false;

        std::function<void(const std::string&)> m_onChange;
        std::function<void()> m_onSubmit;

        // Cached by the most recent measure() call -- same caching
        // model as Text (see text.hpp's own "v1 shaping/caching
        // model"), reshaped every measure() call rather than only on
        // an explicit text change, since WindowUiTree's v1 tick()
        // already re-measures the whole dirty tree wholesale anyway
        // (see its own class comment) -- there is no cheaper "only
        // when text changed" moment to hook into yet. m_shapedGlyphs
        // holds text()'s glyphs; m_placeholderGlyphs holds
        // placeholder()'s, shaped instead of m_shapedGlyphs whenever
        // text() is empty -- see measure()'s own comment for why both
        // need shaping ahead of time (paint() never touches the
        // backend, same as Text's own paint()).
        TextMetrics m_metrics;
        std::vector<ShapedGlyph> m_shapedGlyphs;
        std::vector<ShapedGlyph> m_placeholderGlyphs;

        UiRect m_bounds;

        static constexpr float kHorizontalInset = 8.0f;

        // x offset (from this field's own bounds().x + kHorizontalInset)
        // of the glyph boundary at byte offset `byteOffset` into
        // m_text, using m_shapedGlyphs/m_metrics as measure() last left
        // them. Assumes 1 byte == 1 shaped glyph (see this class's own
        // "v1 scope" -- ASCII only); byteOffset is clamped to
        // m_shapedGlyphs.size(). Always 0 while text() is empty --
        // m_metrics may currently describe the placeholder or a bare
        // space (see measure()), neither of which is `this field's own
        // text advancing`.
        float glyphOffsetForByte(std::size_t byteOffset) const noexcept;

        // Inverse of the above: the byte offset whose glyph boundary is
        // closest to `localX` (already relative to the same origin
        // glyphOffsetForByte() uses) -- used to place the caret on a
        // click.
        std::size_t byteOffsetForLocalX(float localX) const noexcept;
    };
}
