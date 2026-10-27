#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vLocalPos;
layout(location = 2) in vec2 vRectHalfExtent;
layout(location = 3) in float vCornerRadius;
layout(location = 4) in vec2 vTexCoord;
layout(location = 5) in float vTexMode;

// Bound per draw batch (see VulkanUiRenderer::renderFrame()): a 1x1
// white dummy for plain rect batches, an image's own texture for
// ImageCommand batches, or a font's glyph atlas for GlyphRunCommand
// batches. Vulkan requires SOME valid descriptor set to be bound even
// when texMode == 0 never actually samples it.
layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(location = 0) out vec4 outColor;

// Signed distance from `p` to the boundary of an axis-aligned rounded
// box centered at the origin -- the standard formulation (see Inigo
// Quilez's "2D distance functions" article), not project-specific.
// Negative inside, positive outside, ~0 at the (anti-aliased) edge.
float roundedBoxSDF(vec2 p, vec2 halfExtent, float radius)
{
    vec2 q = abs(p) - halfExtent + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

void main()
{
    // vTexMode selects how this fragment gets its color/alpha (a float
    // rather than an int so it interpolates without needing `flat` --
    // it's identical across all vertices of any one quad anyway):
    //   0 -- solid rounded rect (RectCommand): ignore uTexture, use the
    //        SDF edge anti-aliasing below.
    //   1 -- RGBA image (ImageCommand): sample uTexture directly, tinted
    //        by vColor (ImageCommand::tint).
    //   2 -- alpha-only glyph atlas (GlyphRunCommand): uTexture's red
    //        channel IS the glyph's coverage; vColor.rgb is the ink
    //        color (GlyphRunCommand::color), vColor.a scales it further.
    if (vTexMode < 0.5)
    {
        float dist = roundedBoxSDF(vLocalPos, vRectHalfExtent, vCornerRadius);

        // 1px-wide smoothstep band around the edge for cheap
        // anti-aliasing. cornerRadius == 0 still works fine --
        // roundedBoxSDF degrades to a plain box SDF in that case, no
        // special-casing needed.
        float alpha = 1.0 - smoothstep(-1.0, 1.0, dist);

        outColor = vec4(vColor.rgb, vColor.a * alpha);
    }
    else if (vTexMode < 1.5)
    {
        vec4 sampled = texture(uTexture, vTexCoord);
        outColor = vec4(sampled.rgb * vColor.rgb, sampled.a * vColor.a);
    }
    else
    {
        float coverage = texture(uTexture, vTexCoord).r;
        outColor = vec4(vColor.rgb, vColor.a * coverage);
    }
}
