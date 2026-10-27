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
    // One column's header label and fixed pixel width. v1 has no
    // Content/Auto column sizing (mirrors Container's own "v1 sizing
    // limitation", container.hpp) -- every column is an explicit pixel
    // width, chosen by the caller.
    struct ListViewColumn
    {
        std::string title;
        float width = 120.0f;
    };

    // Grid/Table element: a fixed header row of column titles over a
    // vertical list of rows, each row an ordered set of per-column
    // string cells. This is dsbox_ui's table/grid primitive -- the
    // "ListView" terms used elsewhere in this toolkit (Win32's
    // SysListView32, Qt's QTableView/QListView) all describe roughly
    // this shape: a fixed set of columns, N uniform-height rows,
    // single-selection, keyboard navigable.
    //
    // v1 scope:
    //  - No cell editing -- cells are display-only strings. An editable
    //    cell is a natural follow-up (likely by embedding a TextField
    //    per edited cell) but is not this element's job.
    //  - No column resize/reorder/sort by clicking a header -- headers
    //    are painted labels only. A host that wants sorting re-calls
    //    setRows() with already-sorted data.
    //  - No per-cell styling -- every data row shares the same text
    //    color, alternating only by setAlternateRowColor() (row index
    //    parity), with the selected row drawn in setSelectedRowColor()
    //    instead of either.
    //  - Single selection only -- no multi-select/range-select.
    //  - No vertical (or horizontal) scrolling of its own, same as
    //    TextArea/TextField's own "no scrolling" v1 note -- content
    //    taller than this element's own arranged height is clipped at
    //    its bottom edge. Wrap a ListView in a ScrollView
    //    (scroll_view.hpp) for one that scrolls; ScrollView's own
    //    measure() is what makes that combination work (see its class
    //    comment for why a Content-sized child's natural size needs to
    //    reach the viewport uncapped).
    //  - Fixed, uniform row height -- every row (and the header) is
    //    exactly rowHeight() tall; no per-row variable height.
    class DSBOX_UI_API ListView final : public IUiElement, public IFocusTarget
    {
    public:
        explicit ListView(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~ListView() override;

        ListView(const ListView&) = delete;
        ListView& operator=(const ListView&) = delete;

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
        void setFont(FontHandle font) { m_font = font; m_needsReshape = true; }

        // Replaces the whole column set. Column count no longer
        // matching any existing row's own cell count is tolerated --
        // reshape()/paint() simply use whichever of {columns, row
        // cells} runs out first for a given row; callers are expected
        // to keep them in sync themselves.
        void setColumns(std::vector<ListViewColumn> columns) { m_columns = std::move(columns); m_needsReshape = true; }
        const std::vector<ListViewColumn>& columns() const noexcept { return m_columns; }

        // Replaces the whole row set. Clamps (or clears, if `rows` is
        // now empty) the current selection -- see setSelectedIndex()'s
        // own doc for exactly how -- since an index into the OLD row
        // set may no longer be meaningful against the new one.
        void setRows(std::vector<std::vector<std::string>> rows);
        const std::vector<std::vector<std::string>>& rows() const noexcept { return m_rows; }
        std::size_t rowCount() const noexcept { return m_rows.size(); }

        // kInvalidIndex (the default) means "nothing selected".
        static constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

        // Out-of-range (and not kInvalidIndex) is clamped to
        // kInvalidIndex rather than rejected -- same "caller may be
        // reacting against stale data" tolerance WindowUiTree::setFocus()
        // extends to an unknown focus target (window_ui_tree.cpp).
        // Does NOT itself fire onSelectionChanged() -- that only fires
        // for a selection change made BY THE USER (click/arrow keys),
        // the same distinction TextField/TextArea's own setText() makes
        // against onChange().
        void setSelectedIndex(std::size_t index);
        std::size_t selectedIndex() const noexcept { return m_selectedIndex; }

        void setRowHeight(float height) { m_rowHeight = height; }
        float rowHeight() const noexcept { return m_rowHeight; }
        void setHeaderHeight(float height) { m_headerHeight = height; }

        // Pass alpha 0 (UiColor's own default) to disable showing a
        // header row at all -- measure()/paint() then treat
        // headerHeight() as 0 regardless of setHeaderHeight().
        void setShowHeader(bool show) { m_showHeader = show; }

        void setHeaderBackgroundColor(UiColor color) { m_headerBackgroundColor = color; }
        void setHeaderTextColor(UiColor color) { m_headerTextColor = color; }
        void setRowBackgroundColor(UiColor color) { m_rowBackgroundColor = color; }

        // Drawn instead of setRowBackgroundColor() for every other row
        // (index 1, 3, 5, ...) -- alpha 0 (the default) disables
        // alternating, every row then uses setRowBackgroundColor().
        void setAlternateRowColor(UiColor color) { m_alternateRowColor = color; }

        void setSelectedRowColor(UiColor color) { m_selectedRowColor = color; }
        void setTextColor(UiColor color) { m_textColor = color; }
        void setBorderColor(UiColor color) { m_borderColor = color; }
        void setGridLineColor(UiColor color) { m_gridLineColor = color; }

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }

        // Content (the default) sizes this element's own height to
        // exactly fit headerHeight() + rowCount()*rowHeight() -- the
        // usual case for a ListView embedded in a ScrollView, whose own
        // viewport (not this element) is what actually constrains
        // visible height. Pixel/Percent clip the row list instead,
        // same as any other element's explicit height.
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

        void setEnabled(bool enabled) { m_enabled = enabled; }

        // Fires after selectedIndex() changes as a direct result of a
        // click or Up/Down arrow key this element handled -- never for
        // setSelectedIndex(). Same contract as TextField::onChange().
        void setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged)
        {
            m_onSelectionChanged = std::move(onSelectionChanged);
        }

        // Fires on Enter (KeyDownEvent, not OS autorepeat) while a row
        // is selected -- the keyboard equivalent of double-click
        // activation. There is deliberately no pointer double-click
        // detection in v1 (WindowUiTree has no notion of click timing
        // yet); a host wanting mouse activation too can treat any
        // selection change the same way it treats this callback.
        void setOnRowActivated(std::function<void(std::size_t)> onRowActivated)
        {
            m_onRowActivated = std::move(onRowActivated);
        }

    private:
        float headerHeight() const noexcept { return m_showHeader ? m_headerHeight : 0.0f; }

        // Re-shapes every header title and every row cell's text
        // against the current font -- called from measure() whenever
        // m_needsReshape is set (by setFont()/setColumns()/setRows()),
        // never from paint(), same discipline Text/TextField/TextArea
        // all follow (see Text's own "v1 shaping/caching model",
        // text.hpp) of never touching IUiRenderBackend outside
        // measure(). Populates m_metrics/m_headerGlyphs/m_cellGlyphs.
        void reshape(IUiRenderBackend& backend);

        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        std::vector<ListViewColumn> m_columns;
        std::vector<std::vector<std::string>> m_rows;
        std::size_t m_selectedIndex = kInvalidIndex;

        FontHandle m_font = kInvalidFontHandle;

        float m_rowHeight = 28.0f;
        float m_headerHeight = 28.0f;
        bool m_showHeader = true;

        UiColor m_headerBackgroundColor{ 0.90f, 0.90f, 0.90f, 1.0f };
        UiColor m_headerTextColor{ 0.1f, 0.1f, 0.1f, 1.0f };
        UiColor m_rowBackgroundColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        UiColor m_alternateRowColor{ 0.0f, 0.0f, 0.0f, 0.0f }; // disabled by default -- see setAlternateRowColor()
        UiColor m_selectedRowColor{ 0.25f, 0.45f, 0.85f, 0.35f };
        UiColor m_textColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        UiColor m_borderColor{ 0.7f, 0.7f, 0.7f, 1.0f };
        UiColor m_gridLineColor{ 0.85f, 0.85f, 0.85f, 1.0f };

        UiLength m_preferredWidth = UiLength::pixels(360.0f);
        UiLength m_preferredHeight = UiLength::content();

        bool m_enabled = true;

        std::function<void(std::size_t)> m_onSelectionChanged;
        std::function<void(std::size_t)> m_onRowActivated;

        UiRect m_bounds;

        // Shaping cache -- see reshape()'s own doc. Indexed in
        // parallel with m_columns / m_rows; m_cellGlyphs[row][col]
        // mirrors m_rows[row][col] exactly whenever m_needsReshape is
        // false.
        bool m_needsReshape = true;
        TextMetrics m_metrics;
        std::vector<std::vector<ShapedGlyph>> m_headerGlyphs;
        std::vector<std::vector<std::vector<ShapedGlyph>>> m_cellGlyphs;

        static constexpr float kCellInset = 6.0f;
    };
}
