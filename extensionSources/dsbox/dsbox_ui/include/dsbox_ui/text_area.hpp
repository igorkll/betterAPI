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
    // Multi-line, editable text element -- the TextField of text.hpp/
    // text_field.hpp's own "v1 scope" extended with soft word-wrap and
    // multiple lines. Follows TextField's own shape closely (focusable,
    // same PointerDown/KeyDown/TextInputEvent vocabulary) rather than
    // being built out of Text/Container -- wrapping and caret placement
    // both need per-glyph measurement that only a dedicated element
    // with its own measure()/paint() can own, the same reasoning
    // TextField itself documents for staying off Container.
    //
    // v1 scope (mirrors TextField's own "v1 scope" list):
    //  - ASCII only -- m_cursor is a UTF-8 BYTE offset into text(), see
    //    TextField's own note on exactly what that does and doesn't
    //    get right for multi-byte input.
    //  - No selection.
    //  - Explicit '\n' bytes in text() are HARD line breaks. Within
    //    each hard line, measure() additionally SOFT-wraps at word
    //    boundaries (falling back to a mid-word break only when a
    //    single word alone is wider than the available width) to fit
    //    the element's own resolved width -- see wrapLines() in
    //    text_area.cpp. Soft-wrap points are recomputed on every
    //    measure() (same "no cheaper moment to hook into yet" reasoning
    //    TextField's own measure() comment gives) and are NOT stored
    //    back into text() -- text() always returns exactly what was
    //    typed/set, newline-for-newline.
    //  - No vertical (or horizontal) scrolling of its own: content
    //    taller than this element's own arranged height simply paints
    //    past its bottom edge, clipped to bounds() like TextField's own
    //    "no horizontal scrolling" case. Wrap a TextArea in a
    //    ScrollView (scroll_view.hpp) for a field that needs to scroll.
    //  - No caret blink -- solid while focused, invisible otherwise,
    //    same as TextField.
    class DSBOX_UI_API TextArea final : public IUiElement, public IFocusTarget, public ITextInputTarget
    {
    public:
        explicit TextArea(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~TextArea() override;

        TextArea(const TextArea&) = delete;
        TextArea& operator=(const TextArea&) = delete;

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

        // Replaces the whole value and moves the caret to its end --
        // same "does NOT fire onChange()" contract as TextField::setText().
        void setText(std::string text);
        const std::string& text() const noexcept { return m_text; }

        void setPlaceholder(std::string placeholder) { m_placeholder = std::move(placeholder); }

        void setTextColor(UiColor color) { m_textColor = color; }
        void setPlaceholderColor(UiColor color) { m_placeholderColor = color; }
        void setBackgroundColor(UiColor color) { m_backgroundColor = color; }
        void setBorderColor(UiColor color) { m_borderColor = color; }
        void setFocusedBorderColor(UiColor color) { m_focusedBorderColor = color; }
        void setCornerRadius(float radius) { m_cornerRadius = radius; }

        // Extra space between consecutive wrapped lines, on top of
        // whatever the font's own TextMetrics::lineHeight already is.
        void setLineSpacing(float spacing) { m_lineSpacing = spacing; }

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

        void setEnabled(bool enabled) { m_enabled = enabled; }

        // Fires after text() actually changes as a direct result of a
        // TextInputEvent, Backspace, Delete, or Enter this element
        // handled -- never for setText(). Same contract as
        // TextField::setOnChange().
        void setOnChange(std::function<void(const std::string&)> onChange) { m_onChange = std::move(onChange); }

    private:
        struct WrappedLine
        {
            std::size_t startByte = 0; // offset into m_text where this wrapped line starts
            std::size_t endByte = 0;   // one past the last byte this line covers (exclusive)
            float width = 0.0f;
            std::vector<ShapedGlyph> glyphs; // positions relative to this line's own startByte
        };

        void rewrap(IUiRenderBackend& backend, float maxWidth);
        std::size_t lineIndexForByte(std::size_t byteOffset) const noexcept;
        float glyphOffsetForByteInLine(const WrappedLine& line, std::size_t byteOffset) const noexcept;
        std::size_t byteOffsetForLocalXInLine(const WrappedLine& line, float localX) const noexcept;
        void moveCursorVertically(int lineDelta);

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
        float m_lineSpacing = 2.0f;

        UiLength m_preferredWidth = UiLength::pixels(260.0f);
        UiLength m_preferredHeight = UiLength::pixels(120.0f);

        bool m_enabled = true;
        bool m_focused = false;

        std::function<void(const std::string&)> m_onChange;

        // Recomputed by rewrap() (called from measure()) whenever
        // m_text or the resolved width changes -- see this class's own
        // "v1 scope" for why wrapping is recomputed every measure()
        // rather than cached across edits.
        std::vector<WrappedLine> m_lines;
        TextMetrics m_metrics; // one font's metrics, shared by every wrapped line
        std::vector<ShapedGlyph> m_placeholderGlyphs;
        float m_lastWrapWidth = -1.0f;

        UiRect m_bounds;

        static constexpr float kInset = 8.0f;
    };
}
