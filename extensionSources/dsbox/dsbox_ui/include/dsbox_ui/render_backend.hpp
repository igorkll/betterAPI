#pragma once

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/length.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace dsbox_ui
{
    struct TextMetrics
    {
        float ascent = 0.0f;
        float descent = 0.0f;
        float lineHeight = 0.0f;
        float advanceWidth = 0.0f; // total horizontal extent of the measured text
    };

    // Backend contract that WindowUiTree/IUiElement::measure() code
    // depends on, instead of on dsbox_vulkan directly -- see the
    // project-wide split: dsbox_ui knows about this interface only,
    // dsbox_vulkan_gui is (for now) its one implementation, the same
    // way dsbox_vulkan depends on dsbox::IWindowProvider without ever
    // depending on dsbox_sdl.
    //
    // loadFont()/measureText() are expected to be pure CPU work against
    // the font's own metrics/shaping tables -- NOT touching the GPU or
    // an implementation's texture atlas at all. That split matters for
    // WindowUiTree's threading model: measure() (and therefore these
    // two) runs on the control thread as part of an event's
    // synchronous, incremental relayout, while actually rasterizing a
    // glyph into a GPU atlas is deferred to whichever thread later
    // walks the resulting DisplayList (see submit()) -- an
    // implementation that blocked on GPU work inside measureText()
    // would reintroduce exactly the control-thread stall this split
    // was meant to avoid.
    class IUiRenderBackend
    {
    public:
        virtual ~IUiRenderBackend() = default;

        // `sizeInPixels` is the em-size the returned handle is locked
        // to; measuring the same logical font at a different size is a
        // separate loadFont() call and a separate FontHandle.
        virtual FontHandle loadFont(std::string_view path, float sizeInPixels) = 0;

        virtual TextMetrics measureText(FontHandle font, std::string_view utf8) = 0;

        // Shapes `utf8` against `font` into a sequence of positioned
        // glyphs (see ShapedGlyph, display_list.hpp) -- the other CPU-
        // only half of what a Text element needs during measure(), on
        // top of measureText()'s aggregate metrics. Same threading
        // constraint as measureText() above: pure CPU work against the
        // font's own shaping tables, safe to call from the control
        // thread, never touching the GPU or an implementation's own
        // texture atlas. A Text element calls this once per measure()
        // and caches the result for its own later paint() call (see
        // GlyphRunCommand's own comment for why paint() never reshapes).
        virtual std::vector<ShapedGlyph> shapeText(FontHandle font, std::string_view utf8) = 0;

        // Registers image bytes (already decoded, e.g. RGBA8) for later
        // use in an ImageCommand/paint() call. Actual GPU upload may be
        // deferred by the implementation until the image is first
        // encountered in a submitted DisplayList.
        virtual ImageHandle registerImage(
            std::uint32_t width, std::uint32_t height,
            const std::uint8_t* rgba8Pixels) = 0;

        // Consumes one WindowUiTree's frame worth of paint() output.
        // Called from whichever thread owns presentation for the
        // window this list belongs to (the Vulkan thread, for
        // dsbox_vulkan_gui) -- never expected to be called from the
        // control thread; see WindowUiTree's own threading notes for
        // how a DisplayList actually gets from one thread to the
        // other.
        virtual void submit(const DisplayList& list) = 0;
    };

    // Attempts backend.loadFont(path, sizeInPixels); if that returns
    // kInvalidFontHandle -- the font file doesn't exist or can't be
    // read, or (as of dsbox_vulkan_gui's own current placeholder
    // implementation, see VulkanUiRenderer::loadFont()'s own comment)
    // no real font loading exists in this backend at all yet -- falls
    // back to loadFont(fallbackPath, sizeInPixels) instead. Still
    // returns kInvalidFontHandle if the fallback ALSO fails; there is
    // no further fallback after this one.
    //
    // Deliberately takes `fallbackPath` as a required argument rather
    // than defaulting it to some specific font -- a good fallback
    // (Consolas, say) is inherently platform-specific, and dsbox_ui
    // itself has no opinion on what's installed wherever this runs
    // (see the Project brief: general-purpose, not tied to any one
    // environment). Choosing that path is the caller's job -- see
    // dsbox_ui_demo's own main.cpp for an example that picks Consolas
    // specifically because it's reliably present on Windows.
    inline FontHandle loadFontOrFallback(
        IUiRenderBackend& backend, std::string_view path, float sizeInPixels, std::string_view fallbackPath)
    {
        const FontHandle primary = backend.loadFont(path, sizeInPixels);
        if (primary != kInvalidFontHandle)
            return primary;
        return backend.loadFont(fallbackPath, sizeInPixels);
    }
}
