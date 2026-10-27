#pragma once

#include <dsbox_vulkan_gui/api.hpp>
#include <dsbox_vulkan_gui/vulkan_ui_renderer.hpp>

#include <dsbox/window_render_hook.hpp>
#include <dsbox_ui/window_ui_tree.hpp>
#include <dsbox_vulkan/vulkan_module.hpp>

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

namespace dsbox_vulkan_gui
{
    // Bundles VulkanUiRenderer with the acquire/tick/submit/renderFrame/
    // present cycle and implements dsbox::IWindowRenderHook, so a host
    // application registers one instance per window via
    // dsbox_sdl::WindowModule::setRenderHook() instead of writing its
    // own render loop -- WindowModule's own control thread calls
    // onFrame() once per iteration from then on (see
    // IWindowRenderHook's own contract for what that means for
    // threading/latency before using this).
    //
    // Owns its own command pool/buffers -- dsbox_vulkan deliberately
    // owns none of this (see VulkanModule's own class comment).
    //
    // Usage:
    //   auto driver = std::make_unique<VulkanUiWindowDriver>(
    //       vulkanModule, surface, tree, shaderDirectory);
    //   windowModule.setRenderHook(handle, driver.get());
    //   ...
    //   windowModule.setRenderHook(handle, nullptr); // BEFORE destroying driver -- blocks until safe, see its own doc
    //   driver.reset();
    class DSBOX_VULKAN_GUI_API VulkanUiWindowDriver final : public dsbox::IWindowRenderHook
    {
    public:
        // `vulkan`/`surface`/`tree` must all outlive this object.
        // `shaderDirectory`: see VulkanUiRenderer's own constructor.
        // `framesInFlight` must match `vulkan`'s own
        // dsbox_vulkan::VulkanOptions::framesInFlight for `surface`
        // (default 2) -- same requirement as VulkanUiRenderer's own
        // constructor, which this forwards it to.
        VulkanUiWindowDriver(
            dsbox_vulkan::VulkanModule& vulkan,
            dsbox_vulkan::SurfaceHandle surface,
            dsbox_ui::WindowUiTree& tree,
            std::string shaderDirectory,
            std::uint32_t framesInFlight = 2);

        // See this class's own usage note: the caller must have already
        // called WindowModule::setRenderHook(handle, nullptr) for
        // whatever window this was registered against, and had that
        // call RETURN, before destroying this object -- that's what
        // guarantees no onFrame() call is still in flight here.
        ~VulkanUiWindowDriver() override;

        VulkanUiWindowDriver(const VulkanUiWindowDriver&) = delete;
        VulkanUiWindowDriver& operator=(const VulkanUiWindowDriver&) = delete;

        // dsbox::IWindowRenderHook
        void onFrame(dsbox::WindowHandle handle, int width, int height) override;

        // Exposes the dsbox_ui::IUiRenderBackend this driver's render
        // loop feeds every tick()/submit() call to -- so a caller can
        // call loadFont()/registerImage() (see render_backend.hpp,
        // including loadFontOrFallback()) against the EXACT SAME
        // backend instance that will later measure()/paint() this
        // window's tree, and hand the resulting FontHandle/ImageHandle
        // to a Text/Image element already living in that tree. Safe to
        // call right after construction, before the first onFrame() --
        // loadFont()/registerImage() are pure CPU work (see
        // IUiRenderBackend's own threading note) and never touch
        // anything this constructor hasn't already set up.
        dsbox_ui::IUiRenderBackend& renderBackend() noexcept { return m_renderer; }

        std::string lastError() const;

    private:
        dsbox_vulkan::VulkanModule& m_vulkan;
        dsbox_vulkan::SurfaceHandle m_surface;
        dsbox_ui::WindowUiTree& m_tree;

        VulkanUiRenderer m_renderer;

        VkCommandPool m_commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> m_commandBuffers;
        std::uint32_t m_framesInFlight;
        std::uint32_t m_frameIndex = 0;

        // Tracked so a resize is caught on the SAME onFrame() call that
        // first observes it (proactive recreateSwapchain()) rather than
        // waiting for AcquiredFrame::outOfDate on some later call.
        std::uint32_t m_lastKnownWidth = 0;
        std::uint32_t m_lastKnownHeight = 0;

        // Throttles how often a proactive recreateSwapchain() (see
        // above) is allowed to fire during a CONTINUOUS resize drag.
        // recreateSwapchain() itself is cheap now (see its own comment
        // in vulkan_module.cpp -- it only waits on this surface's own
        // fences, not the whole shared device), and with
        // VulkanOptions::presentMode == "fifo" (the default)
        // acquireNextImage() already caps onFrame() to the display's
        // own refresh rate, which coalesces this naturally. This throttle
        // exists for the other presentMode settings ("mailbox"/
        // "immediate"), which are deliberately NOT rate-limited by
        // vsync -- without it, a continuous resize drag under one of
        // those modes would recreate the swapchain (and its image
        // views) far more often than any display could ever show,
        // pure wasted driver work. kResizeRecreateInterval caps that to
        // a still-plenty-smooth cadence regardless of present mode.
        // AcquiredFrame::outOfDate's own recreateSwapchain() call (the
        // exceptional/correctness path, not this proactive one) is
        // deliberately NOT throttled -- see onFrame()'s own comment.
        std::chrono::steady_clock::time_point m_lastResizeRecreate{};
        static constexpr std::chrono::milliseconds kResizeRecreateInterval{ 33 }; // ~30 Hz
    };
}
