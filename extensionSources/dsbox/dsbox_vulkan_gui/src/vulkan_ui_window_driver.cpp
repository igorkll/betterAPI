#include <dsbox_vulkan_gui/vulkan_ui_window_driver.hpp>

namespace dsbox_vulkan_gui
{
    VulkanUiWindowDriver::VulkanUiWindowDriver(
        dsbox_vulkan::VulkanModule& vulkan,
        dsbox_vulkan::SurfaceHandle surface,
        dsbox_ui::WindowUiTree& tree,
        std::string shaderDirectory,
        std::uint32_t framesInFlight)
        : m_vulkan(vulkan)
        , m_surface(surface)
        , m_tree(tree)
        , m_renderer(vulkan, std::move(shaderDirectory), framesInFlight)
        , m_framesInFlight(framesInFlight > 0 ? framesInFlight : 1)
    {
        VkDevice device = vulkan.nativeDevice();
        if (device == VK_NULL_HANDLE)
            return; // VulkanUiRenderer's own constructor already recorded lastError() for this

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = vulkan.graphicsQueueFamily();

        vkCreateCommandPool(device, &poolInfo, nullptr, &m_commandPool);

        m_commandBuffers.resize(m_framesInFlight);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = m_framesInFlight;

        vkAllocateCommandBuffers(device, &allocInfo, m_commandBuffers.data());
    }

    VulkanUiWindowDriver::~VulkanUiWindowDriver()
    {
        // Safe to destroy immediately -- see this class's own header
        // comment: the caller is expected to have already called
        // WindowModule::setRenderHook(handle, nullptr) and had it
        // return, which guarantees no onFrame() call for this driver is
        // still running anywhere.
        VkDevice device = m_vulkan.nativeDevice();
        if (device != VK_NULL_HANDLE && m_commandPool != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);
            vkFreeCommandBuffers(
                device, m_commandPool, static_cast<std::uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
            vkDestroyCommandPool(device, m_commandPool, nullptr);
        }
        // m_renderer is destroyed right after this (member destruction
        // order), which vkDeviceWaitIdle()s again internally -- harmless
        // redundancy, see its own destructor.
    }

    void VulkanUiWindowDriver::onFrame(dsbox::WindowHandle /*handle*/, int width, int height)
    {
        if (width <= 0 || height <= 0)
            return; // minimized, or a transient zero-size -- nothing sensible to draw

        const auto uWidth = static_cast<std::uint32_t>(width);
        const auto uHeight = static_cast<std::uint32_t>(height);

        if (uWidth != m_lastKnownWidth || uHeight != m_lastKnownHeight)
        {
            const auto now = std::chrono::steady_clock::now();

            // First frame ever (m_lastKnownWidth == 0, nothing created
            // yet) always recreates immediately -- only a genuine resize
            // of an already-live swapchain is throttled. Otherwise: only
            // recreate if the throttle interval has actually elapsed.
            // If it hasn't, m_lastKnownWidth/Height are deliberately left
            // UNCHANGED (not updated to uWidth/uHeight) -- so this same
            // branch is entered again next onFrame() call and keeps
            // checking, without ever losing track of "a resize is
            // pending", until the throttle finally allows it through
            // with whatever the size happens to be AT THAT POINT. This
            // naturally coalesces every size this window passed through
            // during the throttled interval into a single recreate.
            const bool firstFrame = (m_lastKnownWidth == 0 && m_lastKnownHeight == 0);

            if (firstFrame || now - m_lastResizeRecreate >= kResizeRecreateInterval)
            {
                m_vulkan.recreateSwapchain(m_surface, uWidth, uHeight);
                m_lastKnownWidth = uWidth;
                m_lastKnownHeight = uHeight;
                m_lastResizeRecreate = now;
            }
        }

        dsbox_vulkan::AcquiredFrame frame = m_vulkan.acquireNextImage(m_surface);

        if (frame.outOfDate)
        {
            // Rare: the size changed again between the proactive
            // recreate above and this acquire, OR this is the first
            // acquire after the throttle above deliberately skipped a
            // recreate. Either way, this is the correctness path, not
            // the steady-state resize path -- deliberately NOT subject
            // to kResizeRecreateInterval above, since acquireNextImage()
            // has already told us the CURRENT swapchain is unusable
            // right now, not just stale. Try again on the very next
            // onFrame() call -- which, since rendering now lives on
            // WindowModule's own control-thread loop rather than a
            // separate thread, happens on the next iteration rather than
            // waiting for a separate thread's own turn.
            m_vulkan.recreateSwapchain(m_surface, uWidth, uHeight);
            m_lastKnownWidth = uWidth;
            m_lastKnownHeight = uHeight;
            m_lastResizeRecreate = std::chrono::steady_clock::now();
            return;
        }

        m_tree.tick(m_renderer, dsbox_ui::UiSize{
            static_cast<float>(frame.extent.width), static_cast<float>(frame.extent.height) });

        if (auto displayList = m_tree.currentDisplayList())
            m_renderer.submit(*displayList);

        VkCommandBuffer cmd = m_commandBuffers[m_frameIndex % m_framesInFlight];

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &beginInfo);

        m_renderer.renderFrame(frame, cmd);

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderFinished;

        // Goes through VulkanModule::submit() rather than calling
        // vkQueueSubmit() on graphicsQueue() directly -- that queue is
        // shared across every window attached to this VulkanModule (see
        // its own class doc), and with each window's onFrame() now
        // running on its own dedicated thread, an unsynchronized direct
        // call here WILL eventually race with another window's submit/
        // present on the same queue. This is not a hypothetical: an
        // earlier version of this file called vkQueueSubmit() directly
        // and crashed inside the NVIDIA driver under exactly that race
        // the first time two windows were actually exercised together.
        m_vulkan.submit(submitInfo, frame.frameCompleteFence);

        if (!m_vulkan.present(m_surface, frame))
            m_vulkan.recreateSwapchain(m_surface, uWidth, uHeight);

        ++m_frameIndex;
    }

    std::string VulkanUiWindowDriver::lastError() const
    {
        return m_renderer.lastError();
    }
}
