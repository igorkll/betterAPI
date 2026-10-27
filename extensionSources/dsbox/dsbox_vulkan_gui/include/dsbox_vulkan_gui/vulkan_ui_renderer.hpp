#pragma once

#include <dsbox_vulkan_gui/api.hpp>

#include <dsbox_ui/display_list.hpp>
#include <dsbox_ui/length.hpp>
#include <dsbox_ui/render_backend.hpp>

#include <dsbox_vulkan/vulkan_module.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dsbox_vulkan_gui
{
    // Implements dsbox_ui::IUiRenderBackend on top of dsbox_vulkan --
    // the one piece of the split agreed for the dsbox_ui/dsbox_vulkan_gui
    // seam: dsbox_ui knows only the IUiRenderBackend contract, never
    // this class or Vulkan itself; this class is (so far) its only
    // implementation.
    //
    // Scope of this version: solid/rounded-corner RectCommands, plus
    // ImageCommands (any RGBA8 image registered via registerImage())
    // and GlyphRunCommands (text via loadFont()/measureText()/
    // shapeText()). PushClip/PopClip is implemented via per-batch
    // scissor rects.
    //
    // Text scope (v1, deliberately minimal): fonts are rasterized with
    // stb_truetype into ONE single-channel glyph atlas per loadFont()
    // call, baked at that call's exact sizeInPixels for printable ASCII
    // only (codepoints 32..126). ShapedGlyph::glyphId is simply that
    // ASCII codepoint (see ShapedGlyph's own doc -- glyph ids are
    // backend-defined and opaque to dsbox_ui), and shapeText()/
    // measureText() walk the input BYTE by BYTE: any byte outside that
    // range -- which includes every byte of a multi-byte UTF-8
    // sequence -- is silently skipped. No kerning, ligatures,
    // bidirectional text, or non-Latin scripts; a real shaping engine
    // (HarfBuzz) is the natural follow-up.
    //
    // Texture scope (v1): images and atlases use LINEAR-tiled, host-
    // visible VkImages written directly from the CPU (no staging
    // buffer, no mipmaps), and are never freed individually -- they
    // live until this renderer is destroyed, in a fixed-size descriptor
    // pool (see the constructor). Fine for a handful of small images
    // and fonts; a follow-up wanting many or large textures should
    // move to OPTIMAL tiling + a staging upload path and add
    // unregister support.
    //
    // One instance is meant to be used for exactly one
    // dsbox_vulkan::SurfaceHandle's frame loop -- same granularity as
    // dsbox_ui::WindowUiTree, and for the same reason (a window's
    // worth of UI is the natural unit on both sides of the seam).
    //
    // Threading: per IUiRenderBackend's own contract (see
    // render_backend.hpp), every method here -- submit() included --
    // is meant to be called from the thread driving this window's
    // presentation loop (the same thread that calls
    // dsbox_vulkan::VulkanModule::acquireNextImage()/present() for this
    // surface), never the control thread. submit() and renderFrame()
    // are meant to be called back-to-back, once each per presented
    // frame, in that order -- see renderFrame()'s own comment for why
    // the frame-in-flight ring buffer requires that exact pairing.
    class DSBOX_VULKAN_GUI_API VulkanUiRenderer final : public dsbox_ui::IUiRenderBackend
    {
    public:
        // `shaderDirectory` must contain rect.vert.spv/rect.frag.spv
        // (see this module's CMakeLists.txt -- it compiles
        // shaders/rect.{vert,frag} via glslc and copies the result next
        // to DSBoxVulkanGui's own built binary, under
        // shaders/dsbox_vulkan_gui/; that path is a reasonable default
        // to pass here during local development, e.g. by resolving it
        // relative to the host application's own executable path).
        //
        // `framesInFlight` MUST match the value the owning
        // dsbox_vulkan::VulkanModule was configured with for this same
        // surface (dsbox_vulkan::VulkanOptions::framesInFlight,
        // default 2) -- this renderer keeps its own ring of
        // `framesInFlight` CPU-visible vertex buffers and relies on
        // VulkanModule::acquireNextImage()'s own fence wait (for the
        // ring slot `framesInFlight` frames ago) to guarantee the GPU
        // is done reading a slot before this renderer overwrites it.
        // There is no way for this renderer to verify that assumption
        // on its own -- a mismatch is a silent race, not a validation
        // error.
        VulkanUiRenderer(dsbox_vulkan::VulkanModule& vulkan, std::string shaderDirectory, std::uint32_t framesInFlight = 2);
        ~VulkanUiRenderer() override;

        VulkanUiRenderer(const VulkanUiRenderer&) = delete;
        VulkanUiRenderer& operator=(const VulkanUiRenderer&) = delete;

        // -- dsbox_ui::IUiRenderBackend --

        // Loads the TrueType file at `path` and bakes its printable-
        // ASCII glyphs at `sizeInPixels` into a GPU atlas -- see the
        // class comment's "Text scope". Returns kInvalidFontHandle (with
        // lastError() set where a Vulkan/stb step failed) if the file
        // is missing/unreadable/not a valid font, or the atlas is too
        // small for that size. Also returns kInvalidFontHandle if the
        // device wasn't ready at construction time.
        dsbox_ui::FontHandle loadFont(std::string_view path, float sizeInPixels) override;

        // Sums each supported glyph's advance (see the class comment's
        // "Text scope" for what "supported" means); ascent/descent/
        // lineHeight come straight from the font's own vertical metrics
        // at the size loadFont() baked. Zeroed TextMetrics{} for an
        // unknown `font`.
        dsbox_ui::TextMetrics measureText(dsbox_ui::FontHandle font, std::string_view utf8) override;

        // One ShapedGlyph per supported byte of `utf8`, xAdvance being
        // that glyph's offset from the run's start. Empty for an
        // unknown `font`.
        std::vector<dsbox_ui::ShapedGlyph> shapeText(dsbox_ui::FontHandle font, std::string_view utf8) override;

        // Uploads `rgba8Pixels` (width * height * 4 bytes, tightly
        // packed, row 0 at the top) to a new GPU texture. Returns
        // kInvalidImageHandle on any failure (bad arguments, or a
        // Vulkan step failed -- lastError() says which).
        dsbox_ui::ImageHandle registerImage(
            std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba8Pixels) override;

        // Builds this frame's vertex/scissor-batch data from `list` on
        // the CPU and uploads it into this frame's ring slot. Does NOT
        // touch a VkCommandBuffer -- see renderFrame() for that half,
        // kept separate for the same reason
        // dsbox_vulkan::AcquiredFrame's own contract keeps acquisition
        // and command recording separate: this renderer has no opinion
        // on when the caller wants to begin/end its render pass or what
        // else might be recorded into the same command buffer.
        void submit(const dsbox_ui::DisplayList& list) override;

        // -- Vulkan-specific (not part of IUiRenderBackend) --

        // Records the geometry the last submit() call uploaded into
        // `commandBuffer`, inside a render pass THIS renderer creates
        // and begins/ends itself against `frame.imageView` (lazily
        // building a VkRenderPass/VkPipeline for `frame.format` the
        // first time it's seen, or if the format ever changes).
        // `commandBuffer` must already be in the recording state
        // (vkBeginCommandBuffer already called) and is left in that
        // same state afterward -- ending it and submitting it to a
        // queue (waiting on frame.imageAvailable, signaling
        // frame.renderFinished, fenced by frame.frameCompleteFence, per
        // AcquiredFrame's own contract) is entirely the caller's job.
        //
        // The render pass this method begins uses
        // VK_ATTACHMENT_LOAD_OP_CLEAR: it assumes it owns the entire
        // image for this frame (a UI-only window, or a UI layer
        // composited by some later frame in the SAME command buffer
        // that draws before this call). Compositing UI on top of a
        // 3D scene rendered by a DIFFERENT renderer into the SAME
        // image within the SAME frame -- which would need LOAD instead
        // of CLEAR, and an explicit hand-off protocol for image layout
        // and synchronization between the two renderers -- is not
        // supported by this method as written; it's a natural
        // follow-up once there's an actual second renderer to design
        // the hand-off against, not something to guess at in the
        // abstract now.
        //
        // Must be called once per presented frame, and must be preceded
        // by a submit() call for the SAME frame's DisplayList -- see
        // the class comment's threading note.
        void renderFrame(const dsbox_vulkan::AcquiredFrame& frame, VkCommandBuffer commandBuffer);

        std::string lastError() const;

    private:
        // Defined in vulkan_ui_renderer.cpp -- holds stb_truetype's own
        // types, which must never appear in this public header.
        struct LoadedFontImpl;

        struct GpuTexture
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE; // combined image sampler, see m_textureSetLayout
        };

        // What a Batch's geometry samples from. None = a plain
        // RectCommand (bound to m_whiteTexture -- see rect.frag).
        struct TextureRef
        {
            enum class Kind { None, Image, Font };

            Kind kind = Kind::None;
            std::uint64_t handle = 0; // ImageHandle or FontHandle, per `kind`

            bool operator==(const TextureRef& other) const noexcept
            {
                return kind == other.kind && handle == other.handle;
            }
        };

        struct Batch
        {
            std::optional<dsbox_ui::UiRect> clip; // nullopt = no clip (full render target)
            TextureRef texture;
            std::uint32_t firstVertex = 0;
            std::uint32_t vertexCount = 0;
        };

        struct FrameSlot
        {
            VkBuffer vertexBuffer = VK_NULL_HANDLE;
            VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
            VkDeviceSize capacityBytes = 0;
            void* mappedPointer = nullptr; // persistently mapped (host-coherent memory)

            std::uint32_t vertexCount = 0;
            std::vector<Batch> batches;
        };

        dsbox_vulkan::VulkanModule& m_vulkan;
        VkDevice m_device = VK_NULL_HANDLE;
        std::string m_shaderDirectory;
        std::uint32_t m_framesInFlight;
        std::uint32_t m_currentFrame = 0;

        VkRenderPass m_renderPass = VK_NULL_HANDLE;
        VkFormat m_renderPassFormat = VK_FORMAT_UNDEFINED;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkPipeline m_pipeline = VK_NULL_HANDLE;

        // Cached rather than created/destroyed per frame: a VkFramebuffer
        // referenced by a render pass instance recorded into a command
        // buffer must stay alive until that command buffer has finished
        // EXECUTING on the GPU, not merely finished recording -- so
        // destroying it right after vkCmdEndRenderPass() (before the
        // caller has even submitted the command buffer) would be a use-
        // after-free from the GPU's point of view. Keyed by the
        // swapchain image's VkImageView, which VulkanModule hands back
        // as a stable identity for a given swapchain generation (see
        // AcquiredFrame::imageView) -- cleared on any extent change
        // (see ensureFramebuffer()'s own comment for why that's safe).
        std::unordered_map<VkImageView, VkFramebuffer> m_framebuffers;
        VkExtent2D m_framebufferExtent{};

        std::vector<FrameSlot> m_frameSlots;

        // -- Texture support (images + font atlases) --

        VkSampler m_linearSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;

        // Fixed-size, created WITHOUT FREE_DESCRIPTOR_SET_BIT: sets are
        // only ever released in bulk when the pool itself is destroyed.
        // See the class comment's "Texture scope".
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;

        // A small dedicated command pool/buffer/fence for the one-off,
        // synchronous layout transition every new texture needs (see
        // transitionImageLayout()) -- deliberately separate from
        // VulkanUiWindowDriver's own per-frame command buffers, since
        // loadFont()/registerImage() must work before any frame exists.
        VkCommandPool m_transferCommandPool = VK_NULL_HANDLE;
        VkCommandBuffer m_transferCommandBuffer = VK_NULL_HANDLE;
        VkFence m_transferFence = VK_NULL_HANDLE;

        GpuTexture m_whiteTexture; // 1x1 opaque white, bound for every plain-rect batch

        // Guards m_images/m_fonts/m_next*Handle AND the m_transfer*
        // objects above: loadFont()/registerImage() may be called from
        // one thread (e.g. app startup) while measureText()/shapeText()
        // (tick) and submit()/renderFrame() (render loop) read the same
        // maps from others. Held only briefly, except during a texture
        // upload, which blocks on m_transferFence.
        mutable std::mutex m_resourceMutex;

        std::unordered_map<dsbox_ui::ImageHandle, GpuTexture> m_images;
        std::unordered_map<dsbox_ui::FontHandle, std::unique_ptr<LoadedFontImpl>> m_fonts;
        dsbox_ui::ImageHandle m_nextImageHandle = 1;
        dsbox_ui::FontHandle m_nextFontHandle = 1;

        mutable std::mutex m_errorMutex;
        std::string m_lastError;

        void setError(std::string message);

        // (Re)builds m_renderPass/m_pipelineLayout/m_pipeline for
        // `colorFormat` if they don't already exist for it. Returns
        // false (with lastError() set) on any Vulkan failure.
        bool ensurePipeline(VkFormat colorFormat);

        // Returns (creating and caching if necessary) the framebuffer
        // for `imageView` at `extent`. See m_framebuffers' own comment
        // for the lifetime reasoning behind caching this at all.
        bool ensureFramebuffer(VkImageView imageView, VkExtent2D extent, VkFramebuffer& outFramebuffer);

        // Grows (destroys + recreates) `slot`'s vertex buffer if it's
        // smaller than `requiredBytes`. Safe to call every frame --
        // a no-op once the slot's capacity has stabilized at its
        // high-water mark.
        bool growVertexBuffer(FrameSlot& slot, VkDeviceSize requiredBytes);

        std::uint32_t findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const;

        void destroyFrameSlot(FrameSlot& slot);

        // Creates a LINEAR-tiled, sampled VkImage + view + descriptor
        // set from `pixels` (`bytesPerPixel` bytes per pixel, tightly
        // packed) and leaves it in SHADER_READ_ONLY_OPTIMAL. Returns
        // false (with lastError() set, and `out` untouched/empty) on
        // any failure.
        bool createGpuTexture(
            std::uint32_t width, std::uint32_t height, VkFormat format,
            const void* pixels, std::size_t bytesPerPixel, GpuTexture& out);

        void destroyGpuTexture(GpuTexture& texture);

        // Synchronous (blocks on m_transferFence): records, submits via
        // VulkanModule::submit() and waits for one image layout barrier.
        bool transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout);
    };
}
