#include <dsbox_vulkan_gui/vulkan_ui_renderer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <variant>

// stb_truetype: single-header, implementation compiled into THIS
// translation unit only (nothing else in the module includes it -- see
// this module's CMakeLists.txt for where the include path comes from).
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

namespace dsbox_vulkan_gui
{
    // Everything about one loaded font that needs stb_truetype's own
    // types -- kept out of the public header on purpose (see
    // LoadedFontImpl's forward declaration there).
    struct VulkanUiRenderer::LoadedFontImpl
    {
        static constexpr int kAtlasWidth = 512;
        static constexpr int kAtlasHeight = 512;
        static constexpr int kFirstChar = 32; // ' '
        static constexpr int kNumChars = 95;  // through '~' (126)

        static bool isSupported(std::uint32_t codepoint)
        {
            return codepoint >= static_cast<std::uint32_t>(kFirstChar) &&
                   codepoint < static_cast<std::uint32_t>(kFirstChar + kNumChars);
        }

        std::vector<unsigned char> ttfBuffer; // stbtt_fontinfo points INTO this -- must outlive `info`
        stbtt_fontinfo info{};
        float ascent = 0.0f;
        float descent = 0.0f;   // positive distance below the baseline
        float lineHeight = 0.0f;
        std::array<stbtt_bakedchar, kNumChars> bakedChars{};
        GpuTexture atlas;
    };

    namespace
    {
        // Matches the vertex input layout declared in ensurePipeline()
        // (VkVertexInputAttributeDescription array) and rect.vert's own
        // `layout(location = N) in ...` declarations -- all three must
        // agree; there is no compile-time check tying them together.
        struct Vertex
        {
            float x = 0.0f, y = 0.0f;                     // pixel-space position
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;  // color
            float centerX = 0.0f, centerY = 0.0f;          // rect center, pixel space (texMode 0 only)
            float halfWidth = 0.0f, halfHeight = 0.0f;     // rect half-extents, pixels (texMode 0 only)
            float cornerRadius = 0.0f;                     // pixels (texMode 0 only)
            float u = 0.0f, v = 0.0f;                      // texture coordinates (texMode 1/2 only)
            float texMode = 0.0f;                          // 0 = solid rect, 1 = RGBA image, 2 = glyph atlas -- see rect.frag
        };

        // Two triangles covering [x0,x1] x [y0,y1] sampling [u0,u1] x
        // [v0,v1] of whatever texture the enclosing Batch binds --
        // shared by images (texMode 1) and glyphs (texMode 2). The
        // rounded-rect fields stay zero: rect.frag only reads them for
        // texMode 0.
        void appendTexturedQuadVertices(
            std::vector<Vertex>& out, float x0, float y0, float x1, float y1,
            float u0, float v0, float u1, float v1, dsbox_ui::UiColor color, float texMode)
        {
            Vertex base;
            base.r = color.r;
            base.g = color.g;
            base.b = color.b;
            base.a = color.a;
            base.texMode = texMode;

            Vertex a = base; a.x = x0; a.y = y0; a.u = u0; a.v = v0;
            Vertex b = base; b.x = x1; b.y = y0; b.u = u1; b.v = v0;
            Vertex c = base; c.x = x1; c.y = y1; c.u = u1; c.v = v1;
            Vertex d = base; d.x = x0; d.y = y1; d.u = u0; d.v = v1;

            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            out.push_back(c);
            out.push_back(d);
            out.push_back(a);
        }

        void appendRectVertices(
            std::vector<Vertex>& out, const dsbox_ui::UiRect& rect, dsbox_ui::UiColor color, float cornerRadius)
        {
            const float left = rect.x;
            const float top = rect.y;
            const float right = rect.x + rect.width;
            const float bottom = rect.y + rect.height;

            Vertex base;
            base.r = color.r;
            base.g = color.g;
            base.b = color.b;
            base.a = color.a;
            base.centerX = rect.x + rect.width * 0.5f;
            base.centerY = rect.y + rect.height * 0.5f;
            base.halfWidth = rect.width * 0.5f;
            base.halfHeight = rect.height * 0.5f;
            base.cornerRadius = cornerRadius;

            Vertex v0 = base; v0.x = left;  v0.y = top;
            Vertex v1 = base; v1.x = right; v1.y = top;
            Vertex v2 = base; v2.x = right; v2.y = bottom;
            Vertex v3 = base; v3.x = left;  v3.y = bottom;

            // Two triangles, CCW in pixel space (top-left origin, Y
            // down) -- irrelevant here since the pipeline disables
            // culling entirely (see ensurePipeline()'s rasterizer
            // state), but kept consistent for anyone reading this later.
            out.push_back(v0);
            out.push_back(v1);
            out.push_back(v2);
            out.push_back(v2);
            out.push_back(v3);
            out.push_back(v0);
        }

        std::vector<char> readSpvFile(const std::string& path)
        {
            std::ifstream file(path, std::ios::ate | std::ios::binary);
            if (!file.is_open())
                return {};

            const std::size_t size = static_cast<std::size_t>(file.tellg());
            std::vector<char> buffer(size);
            file.seekg(0);
            file.read(buffer.data(), static_cast<std::streamsize>(size));
            return buffer;
        }
    }

    VulkanUiRenderer::VulkanUiRenderer(
        dsbox_vulkan::VulkanModule& vulkan, std::string shaderDirectory, std::uint32_t framesInFlight)
        : m_vulkan(vulkan)
        , m_device(vulkan.nativeDevice())
        , m_shaderDirectory(std::move(shaderDirectory))
        , m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1)
    {
        if (m_device == VK_NULL_HANDLE)
        {
            setError("VulkanUiRenderer constructed before VulkanModule::attachToWindow() "
                      "succeeded for any window -- nativeDevice() is still null");
            return; // nothing below can succeed without a device either
        }

        m_frameSlots.resize(m_framesInFlight);

        // -- Texture infrastructure (see the class comment's "Texture
        // scope"). Any failure below returns early with lastError() set
        // and leaves the remaining handles null: ensurePipeline()/
        // loadFont()/registerImage() each check for that rather than
        // assuming this constructor fully succeeded.

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

        if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_linearSampler) != VK_SUCCESS)
        {
            setError("vkCreateSampler failed");
            return;
        }

        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
        setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        setLayoutInfo.bindingCount = 1;
        setLayoutInfo.pBindings = &samplerBinding;

        if (vkCreateDescriptorSetLayout(m_device, &setLayoutInfo, nullptr, &m_textureSetLayout) != VK_SUCCESS)
        {
            setError("vkCreateDescriptorSetLayout failed");
            return;
        }

        // One descriptor set per texture (each image, each font atlas,
        // plus the 1x1 white dummy below), never individually freed --
        // a v1 ceiling of kMaxTextures over this renderer's whole
        // lifetime. Exceeding it makes createGpuTexture() fail with a
        // clear lastError(), not crash.
        constexpr std::uint32_t kMaxTextures = 64;

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = kMaxTextures;

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = kMaxTextures;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            setError("vkCreateDescriptorPool failed");
            return;
        }

        VkCommandPoolCreateInfo transferPoolInfo{};
        transferPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        transferPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        transferPoolInfo.queueFamilyIndex = vulkan.graphicsQueueFamily();

        if (vkCreateCommandPool(m_device, &transferPoolInfo, nullptr, &m_transferCommandPool) != VK_SUCCESS)
        {
            setError("vkCreateCommandPool failed for texture uploads");
            return;
        }

        VkCommandBufferAllocateInfo transferAllocInfo{};
        transferAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        transferAllocInfo.commandPool = m_transferCommandPool;
        transferAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        transferAllocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(m_device, &transferAllocInfo, &m_transferCommandBuffer) != VK_SUCCESS)
        {
            setError("vkAllocateCommandBuffers failed for texture uploads");
            return;
        }

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

        if (vkCreateFence(m_device, &fenceInfo, nullptr, &m_transferFence) != VK_SUCCESS)
        {
            setError("vkCreateFence failed for texture uploads");
            return;
        }

        // 1x1 opaque white, bound for every plain RectCommand batch:
        // Vulkan requires a valid descriptor set to be bound whenever
        // the pipeline's layout declares one, even for draws whose
        // shader path (rect.frag texMode 0) never samples it.
        const std::uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        createGpuTexture(1, 1, VK_FORMAT_R8G8B8A8_UNORM, whitePixel, 4, m_whiteTexture); // lastError() set on failure
    }

    VulkanUiRenderer::~VulkanUiRenderer()
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        vkDeviceWaitIdle(m_device);

        for (auto& slot : m_frameSlots)
            destroyFrameSlot(slot);

        for (auto& [view, framebuffer] : m_framebuffers)
            vkDestroyFramebuffer(m_device, framebuffer, nullptr);

        for (auto& [handle, texture] : m_images)
            destroyGpuTexture(texture);
        for (auto& [handle, font] : m_fonts)
            destroyGpuTexture(font->atlas);
        destroyGpuTexture(m_whiteTexture);

        // Destroying the pool also releases every descriptor set
        // allocated from it -- see m_descriptorPool's own comment.
        if (m_descriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        if (m_textureSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(m_device, m_textureSetLayout, nullptr);
        if (m_linearSampler != VK_NULL_HANDLE)
            vkDestroySampler(m_device, m_linearSampler, nullptr);

        if (m_transferFence != VK_NULL_HANDLE)
            vkDestroyFence(m_device, m_transferFence, nullptr);
        if (m_transferCommandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(m_device, m_transferCommandPool, nullptr); // also frees m_transferCommandBuffer

        if (m_pipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
        if (m_pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
        if (m_renderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
    }

    void VulkanUiRenderer::setError(std::string message)
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = std::move(message);
    }

    std::string VulkanUiRenderer::lastError() const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    dsbox_ui::FontHandle VulkanUiRenderer::loadFont(std::string_view path, float sizeInPixels)
    {
        if (m_device == VK_NULL_HANDLE || m_whiteTexture.descriptorSet == VK_NULL_HANDLE || sizeInPixels <= 0.0f)
            return dsbox_ui::kInvalidFontHandle; // lastError() already set by the constructor, or bad argument

        std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
        if (!file.is_open())
            return dsbox_ui::kInvalidFontHandle; // an expected outcome -- see loadFontOrFallback(); deliberately not a lastError()

        const std::streamsize fileSize = file.tellg();
        if (fileSize <= 0)
            return dsbox_ui::kInvalidFontHandle;

        auto font = std::make_unique<LoadedFontImpl>();
        font->ttfBuffer.resize(static_cast<std::size_t>(fileSize));

        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(font->ttfBuffer.data()), fileSize))
            return dsbox_ui::kInvalidFontHandle;

        const int fontOffset = stbtt_GetFontOffsetForIndex(font->ttfBuffer.data(), 0);
        if (fontOffset < 0 || !stbtt_InitFont(&font->info, font->ttfBuffer.data(), fontOffset))
            return dsbox_ui::kInvalidFontHandle; // not a TrueType/OpenType-glyf font stb can read

        // Bake printable ASCII into one single-channel coverage atlas at
        // exactly `sizeInPixels` -- see the class comment's "Text scope".
        std::vector<unsigned char> atlasPixels(
            static_cast<std::size_t>(LoadedFontImpl::kAtlasWidth) * LoadedFontImpl::kAtlasHeight, 0);

        const int bakeResult = stbtt_BakeFontBitmap(
            font->ttfBuffer.data(), fontOffset, sizeInPixels,
            atlasPixels.data(), LoadedFontImpl::kAtlasWidth, LoadedFontImpl::kAtlasHeight,
            LoadedFontImpl::kFirstChar, LoadedFontImpl::kNumChars, font->bakedChars.data());

        if (bakeResult <= 0)
        {
            // Per stb_truetype.h: <= 0 means not every glyph fit in the
            // atlas at this size (a follow-up could grow the atlas).
            setError("stbtt_BakeFontBitmap: '" + std::string(path) + "' does not fit a " +
                      std::to_string(LoadedFontImpl::kAtlasWidth) + "x" + std::to_string(LoadedFontImpl::kAtlasHeight) +
                      " atlas at " + std::to_string(sizeInPixels) + "px");
            return dsbox_ui::kInvalidFontHandle;
        }

        int ascent = 0, descent = 0, lineGap = 0;
        stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &lineGap);

        // Same scale stbtt_BakeFontBitmap used internally, so these
        // metrics line up with the baked glyph offsets.
        const float scale = stbtt_ScaleForPixelHeight(&font->info, sizeInPixels);
        font->ascent = static_cast<float>(ascent) * scale;
        font->descent = static_cast<float>(-descent) * scale; // stb reports descent as negative
        font->lineHeight = static_cast<float>(ascent - descent + lineGap) * scale;

        {
            std::lock_guard<std::mutex> lock(m_resourceMutex);

            if (!createGpuTexture(
                    LoadedFontImpl::kAtlasWidth, LoadedFontImpl::kAtlasHeight, VK_FORMAT_R8_UNORM,
                    atlasPixels.data(), 1, font->atlas))
                return dsbox_ui::kInvalidFontHandle; // lastError() already set

            const dsbox_ui::FontHandle handle = m_nextFontHandle++;
            m_fonts.emplace(handle, std::move(font));
            return handle;
        }
    }

    dsbox_ui::TextMetrics VulkanUiRenderer::measureText(dsbox_ui::FontHandle font, std::string_view utf8)
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);

        const auto it = m_fonts.find(font);
        if (it == m_fonts.end())
            return {};

        const LoadedFontImpl& loaded = *it->second;

        float advanceWidth = 0.0f;
        for (const unsigned char c : utf8)
        {
            if (LoadedFontImpl::isSupported(c))
                advanceWidth += loaded.bakedChars[c - LoadedFontImpl::kFirstChar].xadvance;
        }

        dsbox_ui::TextMetrics metrics;
        metrics.ascent = loaded.ascent;
        metrics.descent = loaded.descent;
        metrics.lineHeight = loaded.lineHeight;
        metrics.advanceWidth = advanceWidth;
        return metrics;
    }

    std::vector<dsbox_ui::ShapedGlyph> VulkanUiRenderer::shapeText(dsbox_ui::FontHandle font, std::string_view utf8)
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);

        const auto it = m_fonts.find(font);
        if (it == m_fonts.end())
            return {};

        const LoadedFontImpl& loaded = *it->second;

        std::vector<dsbox_ui::ShapedGlyph> glyphs;
        glyphs.reserve(utf8.size());

        float cursor = 0.0f;
        for (const unsigned char c : utf8)
        {
            if (!LoadedFontImpl::isSupported(c))
                continue;

            // glyphId is the ASCII codepoint itself -- see the class
            // comment's "Text scope"; submit() maps it straight back to
            // this font's baked atlas entry.
            glyphs.push_back(dsbox_ui::ShapedGlyph{ static_cast<std::uint32_t>(c), cursor });
            cursor += loaded.bakedChars[c - LoadedFontImpl::kFirstChar].xadvance;
        }

        return glyphs;
    }

    dsbox_ui::ImageHandle VulkanUiRenderer::registerImage(
        std::uint32_t width, std::uint32_t height, const std::uint8_t* rgba8Pixels)
    {
        if (m_device == VK_NULL_HANDLE || m_whiteTexture.descriptorSet == VK_NULL_HANDLE ||
            rgba8Pixels == nullptr || width == 0 || height == 0)
            return dsbox_ui::kInvalidImageHandle;

        std::lock_guard<std::mutex> lock(m_resourceMutex);

        GpuTexture texture;
        if (!createGpuTexture(width, height, VK_FORMAT_R8G8B8A8_UNORM, rgba8Pixels, 4, texture))
            return dsbox_ui::kInvalidImageHandle; // lastError() already set

        const dsbox_ui::ImageHandle handle = m_nextImageHandle++;
        m_images.emplace(handle, texture);
        return handle;
    }

    bool VulkanUiRenderer::transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        vkResetCommandBuffer(m_transferCommandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(m_transferCommandBuffer, &beginInfo) != VK_SUCCESS)
        {
            setError("vkBeginCommandBuffer failed for a texture layout transition");
            return false;
        }

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; // the CPU just wrote this image's memory
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            m_transferCommandBuffer,
            VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        if (vkEndCommandBuffer(m_transferCommandBuffer) != VK_SUCCESS)
        {
            setError("vkEndCommandBuffer failed for a texture layout transition");
            return false;
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &m_transferCommandBuffer;

        // Through VulkanModule::submit() (not vkQueueSubmit directly):
        // the graphics queue is shared with every other window's render
        // thread -- see that method's own thread-safety note.
        if (!m_vulkan.submit(submitInfo, m_transferFence))
        {
            setError("VulkanModule::submit failed for a texture layout transition: " + m_vulkan.lastError());
            return false;
        }

        vkWaitForFences(m_device, 1, &m_transferFence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_device, 1, &m_transferFence);
        return true;
    }

    bool VulkanUiRenderer::createGpuTexture(
        std::uint32_t width, std::uint32_t height, VkFormat format,
        const void* pixels, std::size_t bytesPerPixel, GpuTexture& out)
    {
        GpuTexture texture;

        auto fail = [&](const char* message) -> bool
        {
            setError(message);
            destroyGpuTexture(texture);
            return false;
        };

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        // LINEAR so the CPU can write pixels straight through a mapped
        // pointer -- same no-staging-buffer trade-off as this renderer's
        // vertex buffers, at the cost of a size/format ceiling some GPUs
        // impose on LINEAR images. See the class comment's "Texture scope".
        imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

        if (vkCreateImage(m_device, &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
            return fail("vkCreateImage failed for a UI texture (LINEAR tiling may be unsupported for this format/size)");

        VkMemoryRequirements memRequirements{};
        vkGetImageMemoryRequirements(m_device, texture.image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &texture.memory) != VK_SUCCESS)
            return fail("vkAllocateMemory failed for a UI texture");

        vkBindImageMemory(m_device, texture.image, texture.memory, 0);

        // A LINEAR image's rows are padded to the driver's own pitch
        // (rarely equal to width * bytesPerPixel) -- copy row by row.
        const VkImageSubresource subresource{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(m_device, texture.image, &subresource, &layout);

        void* mapped = nullptr;
        if (vkMapMemory(m_device, texture.memory, 0, memRequirements.size, 0, &mapped) != VK_SUCCESS)
            return fail("vkMapMemory failed for a UI texture");

        const auto* src = static_cast<const std::uint8_t*>(pixels);
        auto* dst = static_cast<std::uint8_t*>(mapped) + layout.offset;
        const std::size_t rowBytes = static_cast<std::size_t>(width) * bytesPerPixel;

        for (std::uint32_t row = 0; row < height; ++row)
            std::memcpy(dst + static_cast<std::size_t>(row) * layout.rowPitch, src + static_cast<std::size_t>(row) * rowBytes, rowBytes);

        vkUnmapMemory(m_device, texture.memory);

        if (!transitionImageLayout(texture.image, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
        {
            destroyGpuTexture(texture);
            return false; // lastError() already set
        }

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texture.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        if (vkCreateImageView(m_device, &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
            return fail("vkCreateImageView failed for a UI texture");

        VkDescriptorSetAllocateInfo setAllocInfo{};
        setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = m_descriptorPool;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &m_textureSetLayout;

        if (vkAllocateDescriptorSets(m_device, &setAllocInfo, &texture.descriptorSet) != VK_SUCCESS)
            return fail("vkAllocateDescriptorSets failed for a UI texture -- the fixed-size descriptor pool is probably exhausted");

        VkDescriptorImageInfo descriptorImage{};
        descriptorImage.sampler = m_linearSampler;
        descriptorImage.imageView = texture.view;
        descriptorImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = texture.descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descriptorImage;

        vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

        out = texture;
        return true;
    }

    void VulkanUiRenderer::destroyGpuTexture(GpuTexture& texture)
    {
        // texture.descriptorSet is deliberately NOT freed here: the pool
        // has no FREE_DESCRIPTOR_SET_BIT (see m_descriptorPool's comment),
        // its sets are reclaimed only when the pool itself is destroyed.
        if (texture.view != VK_NULL_HANDLE)
            vkDestroyImageView(m_device, texture.view, nullptr);
        if (texture.image != VK_NULL_HANDLE)
            vkDestroyImage(m_device, texture.image, nullptr);
        if (texture.memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, texture.memory, nullptr);

        texture = GpuTexture{};
    }

    void VulkanUiRenderer::submit(const dsbox_ui::DisplayList& list)
    {
        using namespace dsbox_ui;

        if (m_device == VK_NULL_HANDLE)
            return; // lastError() already set by the constructor

        std::vector<Vertex> vertices;
        std::vector<Batch> batches;
        std::vector<UiRect> clipStack;
        std::size_t currentBatchIndex = static_cast<std::size_t>(-1);

        auto activeClip = [&]() -> std::optional<UiRect>
        {
            return clipStack.empty() ? std::nullopt : std::optional<UiRect>(clipStack.back());
        };

        auto ensureBatchFor = [&](std::optional<UiRect> clip, TextureRef texture) -> Batch&
        {
            if (currentBatchIndex != static_cast<std::size_t>(-1) &&
                batches[currentBatchIndex].clip == clip && batches[currentBatchIndex].texture == texture)
                return batches[currentBatchIndex];

            batches.push_back(Batch{ clip, texture, static_cast<std::uint32_t>(vertices.size()), 0 });
            currentBatchIndex = batches.size() - 1;
            return batches[currentBatchIndex];
        };

        // m_images/m_fonts are looked up below -- see m_resourceMutex.
        std::lock_guard<std::mutex> resourceLock(m_resourceMutex);

        for (const auto& command : list.commands)
        {
            if (const auto* rect = std::get_if<RectCommand>(&command))
            {
                Batch& batch = ensureBatchFor(activeClip(), TextureRef{});
                const std::size_t before = vertices.size();
                appendRectVertices(vertices, rect->rect, rect->color, rect->cornerRadius);
                batch.vertexCount += static_cast<std::uint32_t>(vertices.size() - before);
            }
            else if (const auto* image = std::get_if<ImageCommand>(&command))
            {
                // An unregistered/invalid handle draws nothing -- same
                // "a renderer isn't the place to reject a bad command"
                // stance as the unbalanced PopClip case below.
                if (m_images.find(image->image) == m_images.end())
                    continue;

                Batch& batch = ensureBatchFor(activeClip(), TextureRef{ TextureRef::Kind::Image, image->image });
                const std::size_t before = vertices.size();
                appendTexturedQuadVertices(
                    vertices,
                    image->rect.x, image->rect.y,
                    image->rect.x + image->rect.width, image->rect.y + image->rect.height,
                    0.0f, 0.0f, 1.0f, 1.0f,
                    image->tint, 1.0f);
                batch.vertexCount += static_cast<std::uint32_t>(vertices.size() - before);
            }
            else if (const auto* run = std::get_if<GlyphRunCommand>(&command))
            {
                const auto fontIt = m_fonts.find(run->font);
                if (fontIt == m_fonts.end())
                    continue;

                const LoadedFontImpl& loaded = *fontIt->second;

                Batch& batch = ensureBatchFor(activeClip(), TextureRef{ TextureRef::Kind::Font, run->font });

                const std::size_t glyphCount = std::min(run->glyphIds.size(), run->glyphOffsets.size());
                for (std::size_t i = 0; i < glyphCount; ++i)
                {
                    const std::uint32_t codepoint = run->glyphIds[i];
                    if (!LoadedFontImpl::isSupported(codepoint))
                        continue;

                    const stbtt_bakedchar& baked = loaded.bakedChars[codepoint - LoadedFontImpl::kFirstChar];

                    // run->y is the BASELINE (see GlyphRunCommand), and
                    // stb's yoff is relative to it (negative above).
                    const float x0 = run->x + run->glyphOffsets[i] + baked.xoff;
                    const float y0 = run->y + baked.yoff;
                    const float x1 = x0 + static_cast<float>(baked.x1 - baked.x0);
                    const float y1 = y0 + static_cast<float>(baked.y1 - baked.y0);

                    const float atlasW = static_cast<float>(LoadedFontImpl::kAtlasWidth);
                    const float atlasH = static_cast<float>(LoadedFontImpl::kAtlasHeight);

                    const std::size_t before = vertices.size();
                    appendTexturedQuadVertices(
                        vertices, x0, y0, x1, y1,
                        static_cast<float>(baked.x0) / atlasW, static_cast<float>(baked.y0) / atlasH,
                        static_cast<float>(baked.x1) / atlasW, static_cast<float>(baked.y1) / atlasH,
                        run->color, 2.0f);
                    batch.vertexCount += static_cast<std::uint32_t>(vertices.size() - before);
                }
            }
            else if (const auto* push = std::get_if<PushClipCommand>(&command))
            {
                clipStack.push_back(push->rect);
            }
            else if (std::holds_alternative<PopClipCommand>(command))
            {
                if (!clipStack.empty())
                    clipStack.pop_back();
                // An unbalanced PopClip (empty stack) is silently
                // ignored rather than treated as an error -- paint()
                // implementations are expected to nest push/pop
                // correctly, but a renderer is the wrong place to
                // enforce that.
            }
        }

        FrameSlot& slot = m_frameSlots[m_currentFrame % m_framesInFlight];

        const VkDeviceSize requiredBytes = static_cast<VkDeviceSize>(vertices.size() * sizeof(Vertex));

        if (requiredBytes > 0 && !growVertexBuffer(slot, requiredBytes))
            return; // lastError() already set

        if (!vertices.empty())
            std::memcpy(slot.mappedPointer, vertices.data(), static_cast<std::size_t>(requiredBytes));

        slot.vertexCount = static_cast<std::uint32_t>(vertices.size());
        slot.batches = std::move(batches);
    }

    bool VulkanUiRenderer::growVertexBuffer(FrameSlot& slot, VkDeviceSize requiredBytes)
    {
        if (slot.capacityBytes >= requiredBytes)
            return true;

        // Safe to destroy the old (too-small) buffer outright rather
        // than allocate-then-swap: see the constructor's comment on
        // `framesInFlight` -- by the time this same ring slot is
        // written to again, VulkanModule::acquireNextImage() has
        // already fenced-waited for whatever frame last read it.
        destroyFrameSlot(slot);

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = requiredBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(m_device, &bufferInfo, nullptr, &slot.vertexBuffer) != VK_SUCCESS)
        {
            setError("vkCreateBuffer failed for a UI vertex buffer");
            return false;
        }

        VkMemoryRequirements memRequirements{};
        vkGetBufferMemoryRequirements(m_device, slot.vertexBuffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(
            memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        if (vkAllocateMemory(m_device, &allocInfo, nullptr, &slot.vertexBufferMemory) != VK_SUCCESS)
        {
            setError("vkAllocateMemory failed for a UI vertex buffer");
            vkDestroyBuffer(m_device, slot.vertexBuffer, nullptr);
            slot.vertexBuffer = VK_NULL_HANDLE;
            return false;
        }

        vkBindBufferMemory(m_device, slot.vertexBuffer, slot.vertexBufferMemory, 0);

        if (vkMapMemory(m_device, slot.vertexBufferMemory, 0, requiredBytes, 0, &slot.mappedPointer) != VK_SUCCESS)
        {
            setError("vkMapMemory failed for a UI vertex buffer");
            vkFreeMemory(m_device, slot.vertexBufferMemory, nullptr);
            vkDestroyBuffer(m_device, slot.vertexBuffer, nullptr);
            slot.vertexBufferMemory = VK_NULL_HANDLE;
            slot.vertexBuffer = VK_NULL_HANDLE;
            return false;
        }

        slot.capacityBytes = requiredBytes;
        return true;
    }

    std::uint32_t VulkanUiRenderer::findMemoryType(std::uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memProperties{};
        vkGetPhysicalDeviceMemoryProperties(m_vulkan.nativePhysicalDevice(), &memProperties);

        for (std::uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) &&
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }

        // Should never happen for HOST_VISIBLE|HOST_COHERENT on any
        // conformant driver -- falling back to index 0 rather than
        // asserting keeps this a "probably renders wrong" failure
        // instead of a crash if it somehow ever does.
        return 0;
    }

    void VulkanUiRenderer::destroyFrameSlot(FrameSlot& slot)
    {
        if (slot.mappedPointer)
        {
            vkUnmapMemory(m_device, slot.vertexBufferMemory);
            slot.mappedPointer = nullptr;
        }
        if (slot.vertexBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(m_device, slot.vertexBuffer, nullptr);
            slot.vertexBuffer = VK_NULL_HANDLE;
        }
        if (slot.vertexBufferMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(m_device, slot.vertexBufferMemory, nullptr);
            slot.vertexBufferMemory = VK_NULL_HANDLE;
        }
        slot.capacityBytes = 0;
    }

    bool VulkanUiRenderer::ensurePipeline(VkFormat colorFormat)
    {
        if (m_pipeline != VK_NULL_HANDLE && m_renderPassFormat == colorFormat)
            return true;

        if (m_textureSetLayout == VK_NULL_HANDLE)
            return false; // the constructor's texture setup failed -- lastError() already says why

        if (m_pipeline != VK_NULL_HANDLE)
        {
            // Format changed under us (rare -- e.g. a surface recreated
            // against a different color space). Not a hot path, same
            // "just wait idle" tradeoff VulkanModule itself makes in
            // recreateSwapchain().
            vkDeviceWaitIdle(m_device);
            vkDestroyPipeline(m_device, m_pipeline, nullptr);
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_pipeline = VK_NULL_HANDLE;
            m_pipelineLayout = VK_NULL_HANDLE;
            m_renderPass = VK_NULL_HANDLE;

            for (auto& [view, framebuffer] : m_framebuffers)
                vkDestroyFramebuffer(m_device, framebuffer, nullptr);
            m_framebuffers.clear();
        }

        // -- Render pass --
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = colorFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // See renderFrame()'s own comment on why CLEAR (rather than
        // LOAD) is correct here: dsbox_vulkan performs no layout
        // transition of its own after acquireNextImage() (see
        // vulkan_module.cpp), so this attachment is treated as
        // UNDEFINED on entry every cycle.
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS)
        {
            setError("vkCreateRenderPass failed");
            return false;
        }

        // -- Shaders --
        std::vector<char> vertSpv = readSpvFile(m_shaderDirectory + "/rect.vert.spv");
        std::vector<char> fragSpv = readSpvFile(m_shaderDirectory + "/rect.frag.spv");

        if (vertSpv.empty() || fragSpv.empty())
        {
            setError("Could not read rect.vert.spv/rect.frag.spv from '" + m_shaderDirectory +
                      "' -- see dsbox_vulkan_gui's CMakeLists.txt for where they're built to");
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
            return false;
        }

        auto createShaderModule = [this](const std::vector<char>& spv) -> VkShaderModule
        {
            VkShaderModuleCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = spv.size();
            info.pCode = reinterpret_cast<const std::uint32_t*>(spv.data());

            VkShaderModule module = VK_NULL_HANDLE;
            vkCreateShaderModule(m_device, &info, nullptr, &module);
            return module;
        };

        VkShaderModule vertModule = createShaderModule(vertSpv);
        VkShaderModule fragModule = createShaderModule(fragSpv);

        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            setError("vkCreateShaderModule failed for a UI shader");
            if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(m_device, fragModule, nullptr);
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
            return false;
        }

        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = vertModule;
        vertStage.pName = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = fragModule;
        fragStage.pName = "main";

        VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

        // -- Vertex input -- mirrors Vertex's layout above and
        // rect.vert's `layout(location = N)` declarations.
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributes[7]{};
        attributes[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, x)) };
        attributes[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, r)) };
        attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, centerX)) };
        attributes[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, halfWidth)) };
        attributes[4] = { 4, 0, VK_FORMAT_R32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, cornerRadius)) };
        attributes[5] = { 5, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, u)) };
        attributes[6] = { 6, 0, VK_FORMAT_R32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, texMode)) };

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 7;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE; // 2D UI -- winding order isn't something paint() implementations should have to think about
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkPushConstantRange pushConstantRange{};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(float) * 2; // viewportSize, see rect.vert

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &m_textureSetLayout; // set 0 = the batch's texture, see rect.frag
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstantRange;

        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            setError("vkCreatePipelineLayout failed");
            vkDestroyShaderModule(m_device, vertModule, nullptr);
            vkDestroyShaderModule(m_device, fragModule, nullptr);
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
            return false;
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;

        const VkResult result =
            vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline);

        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragModule, nullptr);

        if (result != VK_SUCCESS)
        {
            setError("vkCreateGraphicsPipelines failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
            vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            m_renderPass = VK_NULL_HANDLE;
            return false;
        }

        m_renderPassFormat = colorFormat;
        return true;
    }

    bool VulkanUiRenderer::ensureFramebuffer(VkImageView imageView, VkExtent2D extent, VkFramebuffer& outFramebuffer)
    {
        if (extent.width != m_framebufferExtent.width || extent.height != m_framebufferExtent.height)
        {
            // The swapchain was (re)created at a new size -- every
            // cached framebuffer was built against the OLD image views
            // and is stale (some may even reference now-destroyed
            // VkImageViews). Safe to tear all of them down right away:
            // reaching this branch means the caller already has a NEW
            // AcquiredFrame, which is only possible after
            // dsbox_vulkan::VulkanModule::recreateSwapchain() already
            // called vkDeviceWaitIdle() (see vulkan_module.cpp) -- so
            // nothing GPU-side can still be reading these.
            if (!m_framebuffers.empty())
            {
                for (auto& [view, framebuffer] : m_framebuffers)
                    vkDestroyFramebuffer(m_device, framebuffer, nullptr);
                m_framebuffers.clear();
            }

            m_framebufferExtent = extent;
        }

        auto it = m_framebuffers.find(imageView);
        if (it != m_framebuffers.end())
        {
            outFramebuffer = it->second;
            return true;
        }

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = m_renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &imageView;
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS)
        {
            setError("vkCreateFramebuffer failed");
            return false;
        }

        m_framebuffers.emplace(imageView, framebuffer);
        outFramebuffer = framebuffer;
        return true;
    }

    void VulkanUiRenderer::renderFrame(const dsbox_vulkan::AcquiredFrame& frame, VkCommandBuffer commandBuffer)
    {
        if (m_device == VK_NULL_HANDLE || frame.outOfDate)
            return;

        if (!ensurePipeline(frame.format))
            return;

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (!ensureFramebuffer(frame.imageView, frame.extent, framebuffer))
            return;

        VkClearValue clearValue{};
        clearValue.color = { { 0.0f, 0.0f, 0.0f, 0.0f } }; // transparent -- see the class comment on compositing with a separate scene renderer

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = m_renderPass;
        beginInfo.framebuffer = framebuffer;
        beginInfo.renderArea.extent = frame.extent;
        beginInfo.clearValueCount = 1;
        beginInfo.pClearValues = &clearValue;

        vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);

        VkViewport viewport{};
        viewport.width = static_cast<float>(frame.extent.width);
        viewport.height = static_cast<float>(frame.extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        const float pushConstants[2] = {
            static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height)
        };
        vkCmdPushConstants(
            commandBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pushConstants), pushConstants);

        FrameSlot& slot = m_frameSlots[m_currentFrame % m_framesInFlight];

        if (slot.vertexCount > 0)
        {
            // Batches resolve their texture to a descriptor set below
            // -- see m_resourceMutex.
            std::lock_guard<std::mutex> resourceLock(m_resourceMutex);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, &slot.vertexBuffer, &offset);

            for (const Batch& batch : slot.batches)
            {
                if (batch.vertexCount == 0)
                    continue;

                // None (plain rects) and any handle that has since gone
                // missing both fall back to the white dummy texture.
                VkDescriptorSet descriptorSet = m_whiteTexture.descriptorSet;

                if (batch.texture.kind == TextureRef::Kind::Image)
                {
                    const auto it = m_images.find(batch.texture.handle);
                    if (it != m_images.end())
                        descriptorSet = it->second.descriptorSet;
                }
                else if (batch.texture.kind == TextureRef::Kind::Font)
                {
                    const auto it = m_fonts.find(batch.texture.handle);
                    if (it != m_fonts.end())
                        descriptorSet = it->second->atlas.descriptorSet;
                }

                if (descriptorSet == VK_NULL_HANDLE)
                    continue;

                vkCmdBindDescriptorSets(
                    commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);

                VkRect2D scissor{};

                if (batch.clip.has_value())
                {
                    const dsbox_ui::UiRect& clip = *batch.clip;

                    // Clamped to the render target -- a clip rect
                    // extending past the window (or with a negative
                    // origin) is a normal outcome of layout, not an
                    // error this renderer should reject.
                    const float left = std::max(clip.x, 0.0f);
                    const float top = std::max(clip.y, 0.0f);
                    const float right = std::min(clip.x + clip.width, static_cast<float>(frame.extent.width));
                    const float bottom = std::min(clip.y + clip.height, static_cast<float>(frame.extent.height));

                    scissor.offset.x = static_cast<std::int32_t>(left);
                    scissor.offset.y = static_cast<std::int32_t>(top);
                    scissor.extent.width = static_cast<std::uint32_t>(std::max(right - left, 0.0f));
                    scissor.extent.height = static_cast<std::uint32_t>(std::max(bottom - top, 0.0f));

                    // A clip rect that no longer overlaps the render
                    // target at all (e.g. an element positioned during a
                    // resize transient, or simply scrolled/laid out
                    // fully off-window) resolves to a zero-area scissor
                    // here -- nothing in this batch could possibly be
                    // visible, so skip the draw call entirely rather
                    // than issuing one with an empty scissor rect.
                    if (scissor.extent.width == 0 || scissor.extent.height == 0)
                        continue;
                }
                else
                {
                    scissor.extent = frame.extent;
                }

                vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
                vkCmdDraw(commandBuffer, batch.vertexCount, 1, batch.firstVertex, 0);
            }
        }

        vkCmdEndRenderPass(commandBuffer);

        m_currentFrame = (m_currentFrame + 1) % m_framesInFlight;
    }
}
