#pragma once

#include <dsbox_ui/length.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dsbox_ui
{
    // Opaque handle to a font previously loaded via
    // IUiRenderBackend::loadFont() -- see render_backend.hpp. Backend-
    // assigned, same spirit as dsbox::WindowHandle: dsbox_ui holds this
    // and passes it back, never interprets its value.
    using FontHandle = std::uint64_t;
    constexpr FontHandle kInvalidFontHandle = 0;

    // Opaque handle to a GPU-uploadable image resource. Same handle
    // discipline as FontHandle -- assigned and interpreted only by
    // whatever IUiRenderBackend implementation is in use.
    using ImageHandle = std::uint64_t;
    constexpr ImageHandle kInvalidImageHandle = 0;

    struct UiColor
    {
        float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    };

    // One glyph as returned by IUiRenderBackend::shapeText()
    // (render_backend.hpp) -- a glyph id paired with its x-advance
    // from the run's start. Same parallel-array layout GlyphRunCommand
    // itself expects (glyphIds/glyphOffsets) -- a Text element (see
    // text.hpp) calls shapeText() once during its own measure() and
    // caches the result, then simply copies these into a
    // GlyphRunCommand at paint() time (see GlyphRunCommand's own
    // comment for why paint() never reshapes).
    struct ShapedGlyph
    {
        std::uint32_t glyphId = 0;
        float xAdvance = 0.0f; // offset from run start, matches GlyphRunCommand::glyphOffsets
    };

    // A positioned run of already-shaped glyphs against one font, at
    // one size -- the result of an
    // IUiRenderBackend::measureText()/shapeText() call a Text element
    // made during its own measure(). paint() only ever hands the
    // backend glyph ids and positions it already resolved during
    // layout; it never re-shapes text at paint time.
    struct GlyphRunCommand
    {
        FontHandle font = kInvalidFontHandle;
        float x = 0.0f;
        float y = 0.0f; // baseline, not top-left
        UiColor color;
        std::vector<std::uint32_t> glyphIds;
        std::vector<float> glyphOffsets; // per-glyph x-advance from `x`, same length as glyphIds
    };

    struct RectCommand
    {
        UiRect rect;
        UiColor color;
        float cornerRadius = 0.0f;
    };

    struct ImageCommand
    {
        UiRect rect;
        ImageHandle image = kInvalidImageHandle;
        UiColor tint{ 1.0f, 1.0f, 1.0f, 1.0f };
    };

    // Restricts every subsequent command (until the matching PopClip)
    // to `rect`. paint() emits these in properly nested push/pop pairs;
    // DisplayListBuilder does not itself validate nesting -- see its
    // pushClip()/popClip() below.
    struct PushClipCommand
    {
        UiRect rect;
    };

    struct PopClipCommand {};

    using DrawCommand = std::variant<
        RectCommand, ImageCommand, GlyphRunCommand,
        PushClipCommand, PopClipCommand
    >;

    // Flat, backend-agnostic recording of one WindowUiTree's paint()
    // pass for one frame. Built by DisplayListBuilder, consumed by
    // IUiRenderBackend::submit() -- an IUiRenderBackend implementation
    // (dsbox_vulkan_gui) walks `commands` in order and never needs to
    // know which IUiElement produced any given one.
    struct DisplayList
    {
        std::vector<DrawCommand> commands;
    };

    // Append-only builder handed to IUiElement::paint(). A container's
    // paint() calls pushClip()/popClip() around its own children's
    // paint() calls as needed (e.g. to clip to its own content box for
    // overflow); a leaf just appends its own command(s).
    class DisplayListBuilder
    {
    public:
        void rect(const UiRect& r, UiColor color, float cornerRadius = 0.0f)
        {
            m_list.commands.push_back(RectCommand{ r, color, cornerRadius });
        }

        void image(const UiRect& r, ImageHandle handle, UiColor tint = { 1.0f, 1.0f, 1.0f, 1.0f })
        {
            m_list.commands.push_back(ImageCommand{ r, handle, tint });
        }

        void glyphRun(GlyphRunCommand run)
        {
            m_list.commands.push_back(std::move(run));
        }

        void pushClip(const UiRect& r)
        {
            m_list.commands.push_back(PushClipCommand{ r });
        }

        void popClip()
        {
            m_list.commands.push_back(PopClipCommand{});
        }

        // Consumes the builder's contents -- leaves it empty,
        // ready to record the next frame's pass.
        DisplayList take()
        {
            return std::exchange(m_list, DisplayList{});
        }

    private:
        DisplayList m_list;
    };
}
