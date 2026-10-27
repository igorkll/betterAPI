#include "test_framework.hpp"

#include <dsbox_vulkan/vulkan_module.hpp>

#include <dsbox/context.hpp>

#include <dsbox_config/persistent_config_store.hpp>
#include <dsbox_sdl/window_module.hpp>

#include <cstdio>
#include <memory>

namespace
{
    // onInit() must never crash or throw regardless of whether this
    // machine has a Vulkan-capable driver -- it's expected to fail
    // gracefully (leaving nativeInstance() == VK_NULL_HANDLE and
    // lastError() populated) in a headless/no-driver environment. Every
    // other test below checks for that outcome and skips what it
    // structurally can't exercise there.
    void testOnInitSucceedsOrRecordsErrorGracefully()
    {
        dsbox::Context context;
        dsbox_vulkan::VulkanModule vulkan;
        vulkan.onInit(context);

        if (vulkan.nativeInstance() == VK_NULL_HANDLE)
        {
            std::printf(
                "[INFO] %s: no Vulkan instance available here (%s) -- expected "
                "in a headless/no-driver environment\n",
                __func__, vulkan.lastError().c_str());
        }

        // Either way, shutdown must be safe.
        vulkan.onShutdown(context);
    }

    // A malformed "[vulkan]" section must not crash onInit() -- it
    // should just fall back to VulkanOptions{}'s hardcoded defaults for
    // whatever failed to parse. Doesn't need a GPU or display, so this
    // one always actually runs (never skips).
    void testMalformedConfigDoesNotCrash()
    {
        auto config = std::make_shared<dsbox_config::PersistentConfigStore>(
            dsbox_config::PersistentConfigStore::load(
                "[vulkan]\n"
                "device_index = not_a_number\n"
                "frames_in_flight = -5\n"
                "validation = notabool\n",
                ""));

        dsbox::Context context;
        context.provide(config);

        dsbox_vulkan::VulkanModule vulkan;
        vulkan.onInit(context); // must not throw
        vulkan.onShutdown(context);
    }

    // A window not created with WindowGraphicsHint::Vulkan must make
    // attachToWindow() fail cleanly (SDL_Vulkan_CreateSurface itself
    // requires the SDL_WINDOW_VULKAN flag), not crash.
    void testAttachWithoutVulkanHintFails()
    {
        dsbox::Context vulkanContext;
        dsbox_vulkan::VulkanModule vulkan;
        vulkan.onInit(vulkanContext);

        if (vulkan.nativeInstance() == VK_NULL_HANDLE)
        {
            std::printf("[SKIP] %s: no Vulkan instance available (%s)\n",
                __func__, vulkan.lastError().c_str());
            return;
        }

        dsbox::Context sdlContext;
        dsbox_sdl::WindowModule window;
        window.onInit(sdlContext);

        // graphicsHint left at its default (None).
        dsbox_sdl::WindowHandle handle = window.createWindow(dsbox_sdl::WindowOptions{});

        if (handle == dsbox_sdl::kInvalidWindowHandle)
        {
            std::printf("[SKIP] %s: could not open any window here (no display?)\n", __func__);
            window.onShutdown(sdlContext);
            vulkan.onShutdown(vulkanContext);
            return;
        }

        dsbox_vulkan::SurfaceHandle surface = vulkan.attachToWindow(window, handle);

        DSBOX_VULKAN_CHECK(surface == dsbox_vulkan::kInvalidSurfaceHandle);
        DSBOX_VULKAN_CHECK(!vulkan.lastError().empty());

        window.onShutdown(sdlContext);
        vulkan.onShutdown(vulkanContext);
    }

    // The full path: attach a properly-hinted window, then acquire and
    // present a few frames in a row (with a minimal command buffer that
    // only transitions the acquired image to a presentable layout -- no
    // actual drawing). This is the real test of the acquire/submit/
    // present synchronization contract described on AcquiredFrame.
    void testAttachAcquirePresentCycle()
    {
        dsbox::Context vulkanContext;
        dsbox_vulkan::VulkanModule vulkan;
        vulkan.onInit(vulkanContext);

        if (vulkan.nativeInstance() == VK_NULL_HANDLE)
        {
            std::printf("[SKIP] %s: no Vulkan instance available (%s)\n",
                __func__, vulkan.lastError().c_str());
            return;
        }

        dsbox::Context sdlContext;
        dsbox_sdl::WindowModule window;
        window.onInit(sdlContext);

        dsbox_sdl::WindowOptions options;
        options.graphicsHint = dsbox_sdl::WindowGraphicsHint::Vulkan;
        options.width = 320;
        options.height = 240;

        dsbox_sdl::WindowHandle handle = window.createWindow(options);

        if (handle == dsbox_sdl::kInvalidWindowHandle)
        {
            std::printf("[SKIP] %s: could not open any window here (no display?)\n", __func__);
            vulkan.onShutdown(vulkanContext);
            return;
        }

        dsbox_vulkan::SurfaceHandle surface = vulkan.attachToWindow(window, handle);

        if (surface == dsbox_vulkan::kInvalidSurfaceHandle)
        {
            std::printf(
                "[SKIP] %s: could not attach to a window (%s) -- likely no "
                "GPU/display combination available here\n",
                __func__, vulkan.lastError().c_str());
            window.onShutdown(sdlContext);
            vulkan.onShutdown(vulkanContext);
            return;
        }

        dsbox_vulkan::SwapchainInfo info = vulkan.swapchainInfo(surface);
        DSBOX_VULKAN_CHECK(info.imageCount > 0);
        DSBOX_VULKAN_CHECK(info.extent.width > 0);
        DSBOX_VULKAN_CHECK(info.extent.height > 0);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = vulkan.graphicsQueueFamily();
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkCommandPool pool = VK_NULL_HANDLE;
        DSBOX_VULKAN_CHECK(vkCreateCommandPool(vulkan.nativeDevice(), &poolInfo, nullptr, &pool) == VK_SUCCESS);

        for (int frameIndex = 0; frameIndex < 3; ++frameIndex)
        {
            dsbox_vulkan::AcquiredFrame frame = vulkan.acquireNextImage(surface);

            if (frame.outOfDate)
            {
                vulkan.recreateSwapchain(surface, 320, 240);
                continue;
            }

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = pool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            DSBOX_VULKAN_CHECK(
                vkAllocateCommandBuffers(vulkan.nativeDevice(), &allocInfo, &cmd) == VK_SUCCESS);

            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &beginInfo);

            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = frame.image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

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

            DSBOX_VULKAN_CHECK(
                vkQueueSubmit(vulkan.graphicsQueue(), 1, &submitInfo, frame.frameCompleteFence) ==
                VK_SUCCESS);

            vulkan.present(surface, frame);

            vkWaitForFences(vulkan.nativeDevice(), 1, &frame.frameCompleteFence, VK_TRUE, UINT64_MAX);
            vkFreeCommandBuffers(vulkan.nativeDevice(), pool, 1, &cmd);
        }

        vkDestroyCommandPool(vulkan.nativeDevice(), pool, nullptr);

        vulkan.detach(surface);
        window.onShutdown(sdlContext);
        vulkan.onShutdown(vulkanContext);
    }
}

void runVulkanModuleTests()
{
    using namespace dsbox_vulkan_tests;

    runTest("OnInitSucceedsOrRecordsErrorGracefully", testOnInitSucceedsOrRecordsErrorGracefully);
    runTest("MalformedConfigDoesNotCrash", testMalformedConfigDoesNotCrash);
    runTest("AttachWithoutVulkanHintFails", testAttachWithoutVulkanHintFails);
    runTest("AttachAcquirePresentCycle", testAttachAcquirePresentCycle);
}
