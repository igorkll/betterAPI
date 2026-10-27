#include <dsbox_ui/stack_layout.hpp>

#include <algorithm>
#include <vector>

namespace dsbox_ui
{
    namespace
    {
        // 0 for anything that doesn't implement IFlexItem at all, or
        // that does but returns <= 0 for this axis -- both mean
        // "not flexible", see IFlexItem's own doc (element.hpp).
        float flexGrowFor(IUiElement& element, UiAxis axis)
        {
            if (auto* flexItem = dynamic_cast<IFlexItem*>(&element))
                return std::max(0.0f, flexItem->flexGrow(axis));
            return 0.0f;
        }
    }

    void StackLayout::layout(
        std::span<std::unique_ptr<IUiElement>> children,
        const UiRect& contentRect,
        IUiRenderBackend& backend)
    {
        const bool vertical = m_direction == StackDirection::Vertical;
        const UiAxis mainAxis = vertical ? UiAxis::Vertical : UiAxis::Horizontal;

        const float mainOrigin = vertical ? contentRect.y : contentRect.x;
        const float mainExtentTotal = vertical ? contentRect.height : contentRect.width;
        const float crossExtent = vertical ? contentRect.width : contentRect.height;

        // Gaps sit between children regardless of which end up
        // flexible, so the total gap space -- and therefore how much
        // is actually available for any child's own size -- can be
        // settled once, up front, rather than threaded through both
        // passes below.
        std::size_t childCount = 0;
        for (auto& child : children)
            if (child)
                ++childCount;

        const float gapTotal = childCount > 1 ? m_gap * static_cast<float>(childCount - 1) : 0.0f;
        const float mainAvailable = std::max(0.0f, mainExtentTotal - gapTotal);

        // Final main-axis size decided for each child, filled in by
        // whichever of the two measure passes below handles it (only
        // one ever will), consumed by the single arrange() pass at the
        // end. Indexed in parallel with `children`, including null
        // slots -- left at 0 and simply skipped everywhere.
        std::vector<float> mainSizes(children.size(), 0.0f);
        std::vector<float> flexWeights(children.size(), 0.0f);

        // Cross-axis size each child's own measure() reported --
        // captured regardless of m_crossAlign (cheap: it's already
        // sitting in the UiSize measure() just returned), but only
        // ACTED on below when m_crossAlign != Stretch.
        std::vector<float> crossSizes(children.size(), 0.0f);

        // -- Pass 1: measure every NON-flexible child, in order,
        // against whatever's left after earlier non-flexible siblings.
        // This is exactly v1's original (and only) behavior, just
        // skipping any child IFlexItem reports as flexible -- its size
        // is decided in pass 2 instead, once the total free space is
        // actually known.
        float remainingMain = mainAvailable;
        float totalFlexWeight = 0.0f;

        for (std::size_t i = 0; i < children.size(); ++i)
        {
            auto& child = children[i];
            if (!child)
                continue;

            const float weight = flexGrowFor(*child, mainAxis);
            if (weight > 0.0f)
            {
                flexWeights[i] = weight;
                totalFlexWeight += weight;
                continue; // sized in pass 2
            }

            UiConstraints childConstraints;
            childConstraints.minWidth = 0.0f;
            childConstraints.minHeight = 0.0f;

            if (vertical)
            {
                childConstraints.maxWidth = crossExtent;
                childConstraints.maxHeight = std::max(0.0f, remainingMain);
            }
            else
            {
                childConstraints.maxWidth = std::max(0.0f, remainingMain);
                childConstraints.maxHeight = crossExtent;
            }

            const UiSize childSize = child->measure(childConstraints, backend);
            const float mainSize = std::clamp(
                vertical ? childSize.height : childSize.width, 0.0f, std::max(0.0f, remainingMain));

            mainSizes[i] = mainSize;
            crossSizes[i] = std::clamp(vertical ? childSize.width : childSize.height, 0.0f, crossExtent);
            remainingMain -= mainSize;
        }

        // -- Pass 2: whatever's left after every non-flexible child
        // (remainingMain, now final) is divided among flexible
        // children in proportion to their own weight -- the same
        // proportional-sharing rule UiUnit::Fraction's own doc
        // promises (length.hpp). Each flexible child is measured with
        // its share as a TIGHT main-axis constraint: the strategy has
        // already decided this child's main-axis size, there is
        // nothing left for the child's own measure() to negotiate on
        // that axis (the whole point of asking for a flexible share in
        // the first place) -- only the cross axis is still open, same
        // as every non-flexible child above.
        if (totalFlexWeight > 0.0f)
        {
            const float freeSpace = std::max(0.0f, remainingMain);

            for (std::size_t i = 0; i < children.size(); ++i)
            {
                if (flexWeights[i] <= 0.0f)
                    continue;

                auto& child = children[i];
                const float share = freeSpace * (flexWeights[i] / totalFlexWeight);

                UiConstraints childConstraints;

                if (vertical)
                {
                    childConstraints.minWidth = 0.0f;
                    childConstraints.maxWidth = crossExtent;
                    childConstraints.minHeight = share;
                    childConstraints.maxHeight = share;
                }
                else
                {
                    childConstraints.minWidth = share;
                    childConstraints.maxWidth = share;
                    childConstraints.minHeight = 0.0f;
                    childConstraints.maxHeight = crossExtent;
                }

                child->measure(childConstraints, backend); // result deliberately ignored on the main axis -- share IS the answer
                mainSizes[i] = share;
                crossSizes[i] = crossExtent; // a flexible child's cross size is always Stretch's answer, regardless of m_crossAlign -- see crossSizes' own comment
            }
        }
        // totalFlexWeight == 0: no flexible children at all, remainingMain
        // is simply left unused, same as v1's original behavior.

        // -- Final pass: arrange() every child, in original order,
        // using whichever main size the two passes above settled on.
        float cursorMain = mainOrigin;
        bool isFirst = true;

        for (std::size_t i = 0; i < children.size(); ++i)
        {
            auto& child = children[i];
            if (!child)
                continue;

            if (!isFirst)
                cursorMain += m_gap;
            isFirst = false;

            const float mainSize = mainSizes[i];

            // m_crossAlign == Stretch (v1's original, still default,
            // behavior) always uses the full crossExtent at offset 0,
            // exactly as before this enum existed. The other three
            // instead give the child its OWN measured cross size (see
            // crossSizes' own comment) and slide it within crossExtent.
            float crossOffset = 0.0f;
            float crossSize = crossExtent;

            if (m_crossAlign != CrossAlign::Stretch)
            {
                crossSize = crossSizes[i];

                switch (m_crossAlign)
                {
                    case CrossAlign::Start:
                        crossOffset = 0.0f;
                        break;
                    case CrossAlign::Center:
                        crossOffset = std::max(0.0f, (crossExtent - crossSize) * 0.5f);
                        break;
                    case CrossAlign::End:
                        crossOffset = std::max(0.0f, crossExtent - crossSize);
                        break;
                    case CrossAlign::Stretch:
                        break; // unreachable -- handled by the branch above
                }
            }

            const UiRect childRect = vertical
                ? UiRect{ contentRect.x + crossOffset, cursorMain, crossSize, mainSize }
                : UiRect{ cursorMain, contentRect.y + crossOffset, mainSize, crossSize };

            child->arrange(childRect, backend);

            cursorMain += mainSize;
        }
    }

    UiSize StackLayout::measureContent(
        std::span<std::unique_ptr<IUiElement>> children,
        const UiConstraints& constraints,
        IUiRenderBackend& backend)
    {
        const bool vertical = m_direction == StackDirection::Vertical;

        // Outer clamp only -- see this method's own declaration
        // (stack_layout.hpp) and ILayoutStrategy::measureContent()'s
        // own doc (element.hpp): children are measured against as much
        // room as `constraints` allows, never stretched to fill a
        // specific contentRect (there isn't one yet).
        UiConstraints childConstraints;
        childConstraints.minWidth = 0.0f;
        childConstraints.minHeight = 0.0f;
        childConstraints.maxWidth = constraints.maxWidth;
        childConstraints.maxHeight = constraints.maxHeight;

        float mainTotal = 0.0f;
        float crossMax = 0.0f;
        bool isFirst = true;

        for (auto& child : children)
        {
            if (!child)
                continue;

            if (!isFirst)
                mainTotal += m_gap;
            isFirst = false;

            // IFlexItem children have no leftover space to claim here
            // -- see this method's own declaration for why their own
            // measure() result is used exactly like any other child's.
            const UiSize childSize = child->measure(childConstraints, backend);
            mainTotal += vertical ? childSize.height : childSize.width;
            crossMax = std::max(crossMax, vertical ? childSize.width : childSize.height);
        }

        const UiSize natural = vertical
            ? UiSize{ crossMax, mainTotal }
            : UiSize{ mainTotal, crossMax };

        return {
            constraints.clampWidth(natural.width),
            constraints.clampHeight(natural.height)
        };
    }
}
