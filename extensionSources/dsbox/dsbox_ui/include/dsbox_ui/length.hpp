#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dsbox_ui
{
    // How a UiLength's numeric value should be interpreted. This is a
    // STYLE-level concept -- it travels with an element's declared size
    // (padding, width, ...) and is deliberately NOT what measure()
    // receives (see UiConstraints below): a container resolves each
    // child's UiLength against its own already-resolved content box
    // before calling that child's measure(), so no element ever has to
    // know its parent's size just to interpret its own length.
    //
    // Adding a new unit later (e.g. a viewport-relative one, once this
    // needs to think about anything resembling a viewport) is a new
    // enumerator plus a new case in whichever ILayoutStrategy resolves
    // it -- it does not require touching IUiElement or UiConstraints.
    enum class UiUnit
    {
        // Absolute device-independent pixels.
        Pixel,

        // Percentage of the parent's resolved content-box size on the
        // matching axis (width against width, height against height).
        Percent,

        // "Figure it out yourself." Resolved by the layout strategy,
        // typically to Content on the axis being measured, or to
        // "whatever's left" for a flex-like strategy that understands
        // free space.
        Auto,

        // fit-content: resolves to this element's own measure() result
        // on that axis, ignoring the parent's size entirely.
        Content,

        // A fraction ("fr") of the free space along the axis, shared
        // proportionally with sibling Fraction lengths. Only meaningful
        // to a layout strategy that implements free-space distribution
        // (grid/flex) -- StackLayout is the first one that does (via
        // the optional IFlexItem capability an element implements, see
        // element.hpp; Container implements it for whichever axis has
        // this unit). A strategy that doesn't implement free-space
        // distribution is expected to treat this as 0 rather than
        // reject it, so a style using `fr` degrades gracefully under a
        // simpler strategy instead of erroring out.
        Fraction,
    };

    // An unresolved length as declared by an element's style/props.
    // Never appears past the point where a layout strategy resolves a
    // child's box -- see UiConstraints for what measure()/arrange()
    // actually operate on.
    struct UiLength
    {
        float value = 0.0f;
        UiUnit unit = UiUnit::Auto;

        constexpr UiLength() = default;
        constexpr UiLength(float value, UiUnit unit) noexcept
            : value(value), unit(unit)
        {
        }

        static constexpr UiLength pixels(float v) noexcept { return { v, UiUnit::Pixel }; }
        static constexpr UiLength percent(float v) noexcept { return { v, UiUnit::Percent }; }
        static constexpr UiLength fraction(float v) noexcept { return { v, UiUnit::Fraction }; }
        static constexpr UiLength autoLength() noexcept { return { 0.0f, UiUnit::Auto }; }
        static constexpr UiLength content() noexcept { return { 0.0f, UiUnit::Content }; }
    };

    // Resolves a UiLength that is known to be Pixel or Percent against
    // `basis` (the parent's resolved content-box size on the matching
    // axis). Any other unit (Auto/Content/Fraction) has no
    // context-free numeric answer -- resolving those is the layout
    // strategy's job, not this helper's, so callers should branch on
    // `length.unit` themselves before reaching for this function.
    constexpr float resolveAgainst(const UiLength& length, float basis) noexcept
    {
        switch (length.unit)
        {
            case UiUnit::Pixel:
                return length.value;
            case UiUnit::Percent:
                return basis * (length.value / 100.0f);
            default:
                return 0.0f;
        }
    }

    // Resolves a style-level UiLength against the incoming measure()
    // bounds for one axis -- shared by every IUiElement that sizes
    // itself directly from its own UiLength (Container, Image, ...)
    // rather than from its own content (a Text element, which sizes
    // from measureText() instead, has no use for this). Pixel and
    // Percent both resolve exactly; Auto/Content/Fraction have no
    // context-free numeric answer at this layer (that's a layout
    // strategy's job, or -- for Fraction specifically -- IFlexItem's,
    // see element.hpp) so they just fall back to filling the incoming
    // max bound, or min if the max is unbounded. True content-fit
    // sizing needs a real two-pass measure, which no ILayoutStrategy
    // implements yet -- see Container's own v1 sizing limitation
    // comment (container.hpp) for the element this was originally
    // written for.
    inline float resolveLengthDimension(const UiLength& length, float minBound, float maxBound) noexcept
    {
        const bool bounded = std::isfinite(maxBound);

        switch (length.unit)
        {
            case UiUnit::Pixel:
                return std::clamp(length.value, minBound, bounded ? maxBound : (std::max)(minBound, length.value));

            case UiUnit::Percent:
            {
                const float basis = bounded ? maxBound : minBound;
                const float resolved = basis * (length.value / 100.0f);
                return std::clamp(resolved, minBound, bounded ? maxBound : (std::max)(minBound, resolved));
            }

            case UiUnit::Auto:
            case UiUnit::Content:
            case UiUnit::Fraction:
            default:
                return bounded ? maxBound : minBound;
        }
    }

    // Resolved min/max bounds handed to IUiElement::measure(), already
    // in pixels -- units are gone by this point. A layout strategy
    // builds this per child by resolving that child's own UiLength
    // style against the strategy's own already-resolved content box.
    struct UiConstraints
    {
        float minWidth = 0.0f;
        float maxWidth = std::numeric_limits<float>::infinity();
        float minHeight = 0.0f;
        float maxHeight = std::numeric_limits<float>::infinity();

        constexpr float clampWidth(float w) const noexcept
        {
            return std::clamp(w, minWidth, maxWidth);
        }

        constexpr float clampHeight(float h) const noexcept
        {
            return std::clamp(h, minHeight, maxHeight);
        }

        // A constraint with no freedom at all on either axis -- e.g.
        // for arrange()'s final box, where the layout strategy has
        // already decided the exact size and there is nothing left to
        // negotiate.
        static constexpr UiConstraints tight(float width, float height) noexcept
        {
            return { width, width, height, height };
        }
    };

    struct UiSize
    {
        float width = 0.0f;
        float height = 0.0f;
    };

    struct UiPoint
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // Final, resolved box in window-space pixels -- what arrange() and
    // paint() operate on. Never contains a UiLength or a unit; by this
    // point every axis has already been fully resolved by whatever
    // ILayoutStrategy owns the parent.
    struct UiRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        constexpr float left() const noexcept { return x; }
        constexpr float top() const noexcept { return y; }
        constexpr float right() const noexcept { return x + width; }
        constexpr float bottom() const noexcept { return y + height; }

        constexpr bool contains(UiPoint p) const noexcept
        {
            return p.x >= x && p.x < right() && p.y >= y && p.y < bottom();
        }

        constexpr bool operator==(const UiRect& other) const noexcept
        {
            return x == other.x && y == other.y && width == other.width && height == other.height;
        }
    };
}
