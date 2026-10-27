#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/element.hpp>

namespace dsbox_ui
{
    enum class StackDirection
    {
        Vertical,
        Horizontal,
    };

    // How each child is positioned/sized along the axis PERPENDICULAR
    // to the stack's own direction (a horizontal stack's cross axis is
    // vertical, and vice versa). Stretch is v1's original (and still
    // default) behavior -- every child fills the full cross extent
    // regardless of its own measure() result on that axis. The other
    // three instead size the child from its OWN cross-axis measure()
    // result and position that smaller box within the cross extent --
    // e.g. Center is what makes a Text label actually sit in the
    // middle of a fixed-height button rather than pinned to its top
    // edge (see button.hpp).
    enum class CrossAlign
    {
        Stretch,
        Start,
        Center,
        End,
    };

    // Places children one after another along `direction`, separated
    // by `gap` pixels, cross-axis placement governed by `crossAlign`
    // (see CrossAlign's own doc -- defaults to Stretch, v1's original
    // only behavior). This is dsbox_ui's first ILayoutStrategy
    // implementation -- see element.hpp's own comment on why a new
    // arrangement algorithm (flex, grid, ...) is meant to arrive as a
    // sibling class here, not as changes to Container.
    //
    // Free-space distribution: a child that implements IFlexItem (see
    // element.hpp) and returns a positive flexGrow() for this stack's
    // main axis -- e.g. a Container whose main-axis UiLength is
    // UiUnit::Fraction -- does NOT get sized from its own measure()
    // call like every other child. Instead, every non-flexible sibling
    // is measured first (exactly as below), then whatever main-axis
    // space is left over is split among the flexible children in
    // proportion to their own weight, and THAT is what they're
    // measured/arranged with. Multiple flexible siblings therefore
    // divide the leftover space instead of the first one claiming all
    // of it. A stack with no flexible children behaves exactly as
    // before this existed.
    //
    // v1 limitation: a plain UiUnit::Auto child (as opposed to
    // Fraction) does NOT implement IFlexItem and so is still sized
    // straight from its own measure() call against "whatever's left" --
    // meaning the FIRST Auto-sized child among a run of several still
    // claims ALL of whatever's left at the point it's reached, leaving
    // nothing for Auto siblings after it (see Container's own
    // measure()). Give every child an explicit Pixel, Percent, or
    // Fraction size along the stack's main axis if more than one needs
    // to share the remaining space predictably; Fraction is the
    // supported way to do that now, Auto is not.
    class DSBOX_UI_API StackLayout final : public ILayoutStrategy
    {
    public:
        explicit StackLayout(
            StackDirection direction = StackDirection::Vertical, float gap = 0.0f,
            CrossAlign crossAlign = CrossAlign::Stretch)
            : m_direction(direction), m_gap(gap), m_crossAlign(crossAlign)
        {
        }

        void setCrossAlign(CrossAlign align) { m_crossAlign = align; }

        void layout(
            std::span<std::unique_ptr<IUiElement>> children,
            const UiRect& contentRect,
            IUiRenderBackend& backend) override;

        // See ILayoutStrategy::measureContent()'s own doc (element.hpp)
        // for the general contract. This implementation: sums every
        // child's own main-axis measure() result plus the gaps between
        // them for the natural main-axis size, and takes the largest
        // cross-axis measure() result for the natural cross-axis size
        // -- i.e. exactly what layout()'s own "Pass 1" already computes
        // per non-flexible child, just without the actual arrange()
        // calls at the end. m_crossAlign plays no role here (there is
        // no contentRect yet to align within); every child is simply
        // measured against as much room as `constraints` allows.
        UiSize measureContent(
            std::span<std::unique_ptr<IUiElement>> children,
            const UiConstraints& constraints,
            IUiRenderBackend& backend) override;

    private:
        StackDirection m_direction;
        float m_gap;
        CrossAlign m_crossAlign;
    };
}
