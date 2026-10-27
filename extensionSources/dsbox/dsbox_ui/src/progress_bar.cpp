#include <dsbox_ui/progress_bar.hpp>

#include <algorithm>
#include <cmath>

namespace dsbox_ui
{
    ProgressBar::ProgressBar(UiElementId id, UiElementIdMode idMode, std::string name)
        : m_id(id), m_idMode(idMode), m_name(std::move(name))
    {
    }

    // Out-of-line for the same DSBOX_UI_API/MSVC export reason as
    // every other element's destructor in this module -- see
    // Container's own comment (container.cpp).
    ProgressBar::~ProgressBar() = default;

    void ProgressBar::setProgress(float progress)
    {
        m_progress = std::clamp(progress, 0.0f, 1.0f);
    }

    void ProgressBar::advance(float deltaSeconds)
    {
        if (deltaSeconds > 0.0f)
            m_phase += deltaSeconds * m_indeterminateSpeed;
    }

    void ProgressBar::setIndeterminateSegmentFraction(float fraction)
    {
        m_indeterminateSegmentFraction = std::clamp(fraction, 0.05f, 1.0f);
    }

    UiSize ProgressBar::measure(const UiConstraints& constraints, IUiRenderBackend& /*backend*/)
    {
        return {
            resolveLengthDimension(m_preferredWidth, constraints.minWidth, constraints.maxWidth),
            resolveLengthDimension(m_preferredHeight, constraints.minHeight, constraints.maxHeight)
        };
    }

    void ProgressBar::arrange(const UiRect& rect, IUiRenderBackend& /*backend*/)
    {
        m_bounds = rect;
    }

    void ProgressBar::paint(DisplayListBuilder& out) const
    {
        const bool hasArea = m_bounds.width > 0.0f && m_bounds.height > 0.0f;
        if (!hasArea)
            return;

        out.rect(m_bounds, m_trackColor, m_cornerRadius);

        if (m_mode == ProgressBarMode::Determinate)
        {
            const float fillWidth = m_bounds.width * std::clamp(m_progress, 0.0f, 1.0f);
            if (fillWidth > 0.0f)
            {
                const UiRect fillRect{ m_bounds.x, m_bounds.y, fillWidth, m_bounds.height };
                out.rect(fillRect, m_fillColor, m_cornerRadius);
            }
        }
        else // Indeterminate
        {
            // m_phase is unbounded (see its own doc); fold it into a
            // triangle wave over [0, 1] here so the highlight sweeps
            // from the track's left edge to its right edge and back,
            // rather than snapping back to the start each lap -- a
            // smoother, more "busy" reading than a sawtooth would give.
            const float t = m_phase - std::floor(m_phase * 0.5f) * 2.0f; // m_phase mod 2, always in [0, 2)
            const float triangle = (t <= 1.0f) ? t : (2.0f - t);          // 0 -> 1 -> 0 over that period

            const float segmentWidth = m_bounds.width * m_indeterminateSegmentFraction;
            const float travel = std::max(0.0f, m_bounds.width - segmentWidth);
            const float segmentX = m_bounds.x + triangle * travel;

            const UiRect segmentRect{ segmentX, m_bounds.y, segmentWidth, m_bounds.height };

            // The highlight must never paint outside the track's own
            // rounded rect -- clip to it rather than trying to clip a
            // rounded corner by hand.
            out.pushClip(m_bounds);
            out.rect(segmentRect, m_fillColor, m_cornerRadius);
            out.popClip();
        }

        if (m_borderColor.a > 0.0f)
        {
            // Faked stroke-only border, same two-nested-rect trick
            // TextField/TextArea's own paint() use (text_field.cpp) --
            // RectCommand is always filled (display_list.hpp), so a
            // "border" here is just this track rect's own color drawn
            // again, inset, UNDER a border-colored rect already drawn
            // first. Since the track/fill above are already drawn,
            // the simplest equivalent is an outline drawn OVER them at
            // the track's own edge: four thin rects, one per side.
            constexpr float kBorderWidth = 1.0f;
            out.rect(UiRect{ m_bounds.x, m_bounds.y, m_bounds.width, kBorderWidth }, m_borderColor);
            out.rect(UiRect{ m_bounds.x, m_bounds.bottom() - kBorderWidth, m_bounds.width, kBorderWidth }, m_borderColor);
            out.rect(UiRect{ m_bounds.x, m_bounds.y, kBorderWidth, m_bounds.height }, m_borderColor);
            out.rect(UiRect{ m_bounds.right() - kBorderWidth, m_bounds.y, kBorderWidth, m_bounds.height }, m_borderColor);
        }
    }
}
