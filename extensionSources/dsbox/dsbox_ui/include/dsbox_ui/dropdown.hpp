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
    enum class DropdownMode
    {
        // A closed, single-line header showing the selected item's own
        // text; clicking it (or Enter/Space while focused) toggles an
        // item list open beneath it. This is the "Dropdown"/"Combobox"
        // half of this class.
        ComboBox,

        // No header, no open/closed state at all -- every item is
        // always shown, in a fixed-height list. This is the "ListBox"
        // half of this class: a ComboBox and a ListBox are the exact
        // same selection model (one list of string items, one
        // selectedIndex, the same keyboard/pointer vocabulary) and
        // differ only in whether the list is ever collapsed -- so
        // rather than a second class duplicating all of that, a
        // ListBox IS a Dropdown constructed with this mode.
        ListBox,
    };

    // Single-selection dropdown/combobox/list-box element -- one
    // header line (ComboBox mode only) plus a list of selectable
    // string items, either toggled open/closed (ComboBox) or always
    // visible (ListBox, see DropdownMode's own doc).
    //
    // v1 scope/limitations:
    //  - No popup/overlay layer anywhere in dsbox_ui yet -- WindowUiTree
    //    paints exactly one tree, in tree order, with no notion of a
    //    "float above everything else regardless of tree position"
    //    layer (that would need its own dedicated support in
    //    WindowUiTree/DisplayList, neither of which has it today). An
    //    OPEN ComboBox therefore expands INLINE -- this element's own
    //    measure() reports a taller box while open, which pushes
    //    whatever is arranged after it (a later StackLayout sibling,
    //    say) further down, exactly the way an inline <select
    //    multiple> or an <details>/<summary> disclosure affects
    //    surrounding HTML layout, rather than the floating overlay
    //    panel a native OS combobox draws. A caller that specifically
    //    needs floating-over-everything-else behavior has nothing in
    //    dsbox_ui to build that with yet.
    //  - No text filtering/typeahead -- selection is by pointer click
    //    or Up/Down + Enter only.
    //  - A fixed, uniform item height (setItemHeight()) -- no per-item
    //    variable height.
    //  - setMaxVisibleItems() caps how many items measure() accounts
    //    for; there is no scrolling of its own for the rest -- wrap
    //    this element's open list in a ScrollView (scroll_view.hpp) if
    //    it needs to hold more items than comfortably fit, the same
    //    "wrap it in a ScrollView" answer TextArea/ListView give for
    //    their own "no scrolling" v1 note. (A ComboBox that's also
    //    wrapped in a ScrollView combines two inline-affecting
    //    behaviors -- workable, just worth calling out.)
    //  - Hover highlight tracks PointerMoveEvent locally (this
    //    element's own handleEvent(), see its own comment) rather than
    //    through any general hover-enter/leave mechanism -- dsbox_ui
    //    has none yet (see Container::setPressedColor()'s own note,
    //    container.hpp, on the exact same gap).
    class DSBOX_UI_API Dropdown final : public IUiElement, public IFocusTarget
    {
    public:
        explicit Dropdown(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~Dropdown() override;

        Dropdown(const Dropdown&) = delete;
        Dropdown& operator=(const Dropdown&) = delete;

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

        // -- Configuration --
        void setMode(DropdownMode mode) { m_mode = mode; m_open = (mode == DropdownMode::ListBox); }
        DropdownMode mode() const noexcept { return m_mode; }

        void setFont(FontHandle font) { m_font = font; m_needsReshape = true; }

        void setItems(std::vector<std::string> items);
        const std::vector<std::string>& items() const noexcept { return m_items; }

        static constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

        // Does NOT itself fire onSelectionChanged() -- same "only for a
        // change made BY THE USER" contract every other element in this
        // module uses (TextField::setText(), ListView::setSelectedIndex(), ...).
        void setSelectedIndex(std::size_t index);
        std::size_t selectedIndex() const noexcept { return m_selectedIndex; }

        // Shown in the header (ComboBox mode only) whenever
        // selectedIndex() == kInvalidIndex.
        void setPlaceholder(std::string placeholder) { m_placeholder = std::move(placeholder); m_needsReshape = true; }

        // ComboBox mode only -- a no-op in ListBox mode (always open,
        // see DropdownMode's own doc).
        void setOpen(bool open);
        bool isOpen() const noexcept { return m_open; }

        void setItemHeight(float height) { m_itemHeight = height; }
        void setHeaderHeight(float height) { m_headerHeight = height; }
        void setMaxVisibleItems(std::size_t count) { m_maxVisibleItems = count; }

        void setHeaderBackgroundColor(UiColor color) { m_headerBackgroundColor = color; }
        void setItemBackgroundColor(UiColor color) { m_itemBackgroundColor = color; }
        void setHoveredItemColor(UiColor color) { m_hoveredItemColor = color; }
        void setSelectedItemColor(UiColor color) { m_selectedItemColor = color; }
        void setTextColor(UiColor color) { m_textColor = color; }
        void setPlaceholderColor(UiColor color) { m_placeholderColor = color; }
        void setBorderColor(UiColor color) { m_borderColor = color; }
        void setFocusedBorderColor(UiColor color) { m_focusedBorderColor = color; }
        void setCornerRadius(float radius) { m_cornerRadius = radius; }

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }

        void setEnabled(bool enabled) { m_enabled = enabled; }

        // Fires after selectedIndex() changes as a direct result of a
        // click or Up/Down+Enter this element handled -- never for
        // setSelectedIndex().
        void setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged)
        {
            m_onSelectionChanged = std::move(onSelectionChanged);
        }

        // Fires whenever isOpen() changes (ComboBox mode only) as a
        // direct result of user interaction.
        void setOnOpenChanged(std::function<void(bool)> onOpenChanged) { m_onOpenChanged = std::move(onOpenChanged); }

    private:
        void reshape(IUiRenderBackend& backend);
        float listHeight() const noexcept;
        float headerHeightResolved() const noexcept { return m_mode == DropdownMode::ComboBox ? m_headerHeight : 0.0f; }

        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        DropdownMode m_mode = DropdownMode::ComboBox;
        bool m_open = false;

        std::vector<std::string> m_items;
        std::size_t m_selectedIndex = kInvalidIndex;
        std::size_t m_hoveredIndex = kInvalidIndex;
        std::string m_placeholder;

        FontHandle m_font = kInvalidFontHandle;

        float m_itemHeight = 26.0f;
        float m_headerHeight = 32.0f;
        std::size_t m_maxVisibleItems = 6;

        UiColor m_headerBackgroundColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        UiColor m_itemBackgroundColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        UiColor m_hoveredItemColor{ 0.92f, 0.92f, 0.95f, 1.0f };
        UiColor m_selectedItemColor{ 0.25f, 0.45f, 0.85f, 0.35f };
        UiColor m_textColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        UiColor m_placeholderColor{ 0.55f, 0.55f, 0.55f, 1.0f };
        UiColor m_borderColor{ 0.7f, 0.7f, 0.7f, 1.0f };
        UiColor m_focusedBorderColor{ 0.25f, 0.45f, 0.85f, 1.0f };
        float m_cornerRadius = 4.0f;

        UiLength m_preferredWidth = UiLength::pixels(220.0f);

        bool m_enabled = true;
        bool m_focused = false;

        std::function<void(std::size_t)> m_onSelectionChanged;
        std::function<void(bool)> m_onOpenChanged;

        bool m_needsReshape = true;
        TextMetrics m_metrics;
        std::vector<std::vector<ShapedGlyph>> m_itemGlyphs;
        std::vector<ShapedGlyph> m_selectedLabelGlyphs; // header label, ComboBox mode -- selected item's own text, reshaped on demand (see reshape())
        std::vector<ShapedGlyph> m_placeholderGlyphs;

        UiRect m_bounds;

        static constexpr float kInset = 8.0f;
    };
}
