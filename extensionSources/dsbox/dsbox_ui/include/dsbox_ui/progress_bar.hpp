#pragma once

#include <dsbox_ui/api.hpp>
#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/element.hpp>
#include <dsbox_ui/length.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <string>

namespace dsbox_ui
{
    enum class ProgressBarMode
    {
        // A fixed-width fill proportional to progress() -- the usual
        // "N% done" bar, for any operation whose total extent is
        // known.
        Determinate,

        // No known total extent -- a highlight segment sweeps back and
        // forth across the track instead of filling it, the same
        // visual convention a native OS "busy"/indeterminate progress
        // bar uses. Driven by advance() -- see its own doc.
        Indeterminate,
    };

    // Non-interactive progress/busy indicator: a track rect with
    // either a proportional fill (Determinate) or a sweeping highlight
    // (Indeterminate) -- see ProgressBarMode's own doc for exactly
    // which. Unlike every other element in this module, ProgressBar
    // implements neither IFocusTarget nor any pointer/keyboard
    // reaction in handleEvent() (always returns false, same as Text's
    // own "not interactive in v1" -- text.hpp): a progress indicator is
    // something an application DRIVES, not something the person
    // interacts with.
    //
    // v1 scope -- no built-in clock/timer: nothing in dsbox_ui owns a
    // clock reachable from an IUiElement (WindowUiTree::tick() itself
    // takes no delta-time -- it only re-measures/re-paints whatever is
    // already marked dirty, see its own class comment, window_ui_tree.hpp).
    // Animating Indeterminate mode is therefore the SAME "whatever owns
    // this element must drive it" contract Text::setText() already
    // documents for its own non-self-repainting setters (text.hpp): a
    // host application calls advance(deltaSeconds) from its own frame
    // loop (wherever it already knows a delta time -- e.g. right before
    // calling WindowUiTree::tick()) to move the animation forward, then
    // calls WindowUiTree::markDirty(id()) itself, same as any other
    // purely-external state change this toolkit's elements don't
    // self-repaint on. A ProgressBar left in Indeterminate mode but
    // never advance()d simply shows its highlight frozen in place --
    // not a bug, just nothing driving it.
    class DSBOX_UI_API ProgressBar final : public IUiElement
    {
    public:
        explicit ProgressBar(UiElementId id, UiElementIdMode idMode = UiElementIdMode::Instance, std::string name = {});
        ~ProgressBar() override;

        ProgressBar(const ProgressBar&) = delete;
        ProgressBar& operator=(const ProgressBar&) = delete;

        // -- IUiElement --
        UiElementId id() const noexcept override { return m_id; }
        UiElementIdMode idMode() const noexcept override { return m_idMode; }
        std::string_view name() const noexcept override { return m_name; }

        UiSize measure(const UiConstraints& constraints, IUiRenderBackend& backend) override;
        void arrange(const UiRect& rect, IUiRenderBackend& backend) override;
        UiRect bounds() const noexcept override { return m_bounds; }
        void paint(DisplayListBuilder& out) const override;

        // Always returns false -- see this class's own class comment
        // on why a progress indicator isn't interactive.
        bool handleEvent(const UiEvent&, IUiElementActions&) override { return false; }

        // Always empty -- a leaf, same as Text/Image.
        std::span<std::unique_ptr<IUiElement>> children() override { return {}; }

        // -- Configuration --
        void setMode(ProgressBarMode mode) { m_mode = mode; }
        ProgressBarMode mode() const noexcept { return m_mode; }

        // Clamped into [0, 1]. Meaningless (ignored by paint()) outside
        // Determinate mode, but still stored/returned as given, so
        // switching modes back and forth never loses it.
        void setProgress(float progress);
        float progress() const noexcept { return m_progress; }

        // See this class's own class comment for the full contract --
        // in short: the host calls this once per frame with elapsed
        // time, then marks this element dirty itself. A no-op outside
        // Indeterminate mode (the phase still advances internally, so
        // switching back into Indeterminate later resumes mid-sweep
        // rather than snapping to the start, but paint() never reads it
        // in Determinate mode either way).
        void advance(float deltaSeconds);

        // Fraction (0..1) of the track's own width the sweeping
        // highlight covers, in Indeterminate mode. Default 0.3 (30%).
        void setIndeterminateSegmentFraction(float fraction);

        // Full back-and-forth sweeps per second, in Indeterminate mode.
        void setIndeterminateSpeed(float sweepsPerSecond) { m_indeterminateSpeed = sweepsPerSecond; }

        void setTrackColor(UiColor color) { m_trackColor = color; }
        void setFillColor(UiColor color) { m_fillColor = color; }
        void setBorderColor(UiColor color) { m_borderColor = color; }
        void setCornerRadius(float radius) { m_cornerRadius = radius; }

        void setPreferredWidth(UiLength width) { m_preferredWidth = width; }
        void setPreferredHeight(UiLength height) { m_preferredHeight = height; }

    private:
        UiElementId m_id;
        UiElementIdMode m_idMode;
        std::string m_name;

        ProgressBarMode m_mode = ProgressBarMode::Determinate;
        float m_progress = 0.0f;

        // Phase accumulates deltaSeconds * m_indeterminateSpeed,
        // unbounded (never wrapped) -- paint() is the one place that
        // folds it into a bounded sweep position, via a triangle wave
        // (see its own comment), so advance() itself never needs to
        // know the wave's own period.
        float m_phase = 0.0f;
        float m_indeterminateSpeed = 0.6f;
        float m_indeterminateSegmentFraction = 0.3f;

        UiColor m_trackColor{ 0.88f, 0.88f, 0.88f, 1.0f };
        UiColor m_fillColor{ 0.25f, 0.55f, 0.85f, 1.0f };
        UiColor m_borderColor{ 0.0f, 0.0f, 0.0f, 0.0f }; // transparent -- no border unless explicitly set
        float m_cornerRadius = 4.0f;

        UiLength m_preferredWidth = UiLength::pixels(200.0f);
        UiLength m_preferredHeight = UiLength::pixels(8.0f);

        UiRect m_bounds;
    };
}
