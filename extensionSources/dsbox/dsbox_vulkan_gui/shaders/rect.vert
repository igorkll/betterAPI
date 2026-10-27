#version 450

// pixel-space -> NDC. Vulkan's default NDC already has +Y pointing down,
// same as our pixel space (origin top-left) -- no flip needed, unlike
// the OpenGL convention some tutorials assume.
layout(push_constant) uniform PushConstants
{
    vec2 viewportSize; // pixels
} pc;

layout(location = 0) in vec2 inPosition;         // pixel space
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inRectCenter;       // pixel space, same for all 6 verts of one rect (texMode 0 only)
layout(location = 3) in vec2 inRectHalfExtent;   // pixels (texMode 0 only)
layout(location = 4) in float inCornerRadius;    // pixels (texMode 0 only)
layout(location = 5) in vec2 inTexCoord;         // [0,1] (texMode 1/2 only)
layout(location = 6) in float inTexMode;         // 0 = solid rect, 1 = RGBA image, 2 = alpha-only glyph atlas

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vLocalPos;       // position relative to rect center, pixels
layout(location = 2) out vec2 vRectHalfExtent;
layout(location = 3) out float vCornerRadius;
layout(location = 4) out vec2 vTexCoord;
layout(location = 5) out float vTexMode;

void main()
{
    vec2 ndc = (inPosition / pc.viewportSize) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    vColor = inColor;
    vLocalPos = inPosition - inRectCenter;
    vRectHalfExtent = inRectHalfExtent;
    vCornerRadius = inCornerRadius;
    vTexCoord = inTexCoord;
    vTexMode = inTexMode;
}
