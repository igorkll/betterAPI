#pragma once

#include <dsbox_vulkan/api.hpp>

#include <dsbox/module.hpp>
#include <dsbox/window_provider.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dsbox_vulkan
{
    using SurfaceHandle = std::uint64_t;
    constexpr SurfaceHandle kInvalidSurfaceHandle = 0;

    // Configuration knobs for VulkanModule. Every field has a sane
    // hardcoded fallback (used when no PersistentConfigStore is
    // reachable via Context, or a given key is simply absent from it),
    // but VulkanModule::onInit() always tries a "[vulkan]" section in
    // Context::get<dsbox_config::PersistentConfigStore>() first -- see
    // the .cpp -- so a host never has to recompile this module just to
    // flip validation or vsync on or off. dsbox_config itself is only a
    // private, .cpp-level dependency: nothing in this public header
    // mentions it.
    struct VulkanOptions
    {
        // "[vulkan] validation" (bool, default false). Enables
        // VK_LAYER_KHRONOS_validation and VK_EXT_debug_utils, but only
        // if the layer actually turns out to be present on this
        // machine -- never a hard failure either way, see lastError().
        bool enableValidation = false;

        // "[vulkan] device_index" (integer, default unset). If >= 0,
        // forces this index into vkEnumeratePhysicalDevices()'s result.
        // Takes priority over preferredDeviceName.
        int preferredDeviceIndex = -1;

        // "[vulkan] device_name" (string, default empty). Case-
        // insensitive substring match against
        // VkPhysicalDeviceProperties::deviceName. Empty means no
        // preference: prefer a discrete GPU, else take whatever the
        // first suitable device is.
        std::string preferredDeviceName;

        // "[vulkan] present_mode" (string, default "fifo"). One of:
        //   "fifo"      -- always supported, capped at display refresh
        //                  rate, no tearing. Safe default.
        //   "mailbox"   -- low-latency triple buffering; silently falls
        //                  back to fifo if the surface doesn't support it.
        //   "immediate" -- uncapped, may tear.
        std::string presentMode = "fifo";

        // "[vulkan] frames_in_flight" (integer >= 1, default 2). How
        // many frames' worth of CPU-side sync objects to keep, so the
        // CPU can start recording frame N+1 while the GPU is still
        // working on frame N.
        std::uint32_t framesInFlight = 2;
    };

    // A single acquired swapchain image, ready to be rendered into and
    // then presented. VulkanModule owns creating/destroying the sync
    // objects below; the CALLER owns everything about how the image
    // actually gets drawn into -- record and submit a command buffer
    // that:
    //   - waits on `imageAvailable` (as a wait semaphore) before the
    //     stage that writes to `image`/`imageView`,
    //   - signals `renderFinished` when done,
    //   - is submitted with `frameCompleteFence` as the submission's
    //     fence.
    // Then pass this same AcquiredFrame to present(). This is the
    // entire contract -- VulkanModule has no idea whether that command
    // buffer contains a game world, a GUI, both in one render pass, or
    // several render passes composited together; that policy is
    // entirely up to whatever calls acquireNextImage().
    struct AcquiredFrame
    {
        SurfaceHandle surface = kInvalidSurfaceHandle;
        std::uint32_t imageIndex = 0;
        VkImage image = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};

        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkSemaphore renderFinished = VK_NULL_HANDLE;
        VkFence frameCompleteFence = VK_NULL_HANDLE;

        // True if the swapchain no longer matches the window (most
        // often mid-resize) -- every field above except `surface` is
        // meaningless in this case. Call recreateSwapchain() and try
        // acquiring again; do not attempt to render or present this
        // frame. Also true if `surface` was already unknown when
        // acquireNextImage() was called.
        bool outOfDate = false;
    };

    struct SwapchainInfo
    {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        std::uint32_t imageCount = 0;
    };

    // Owns exactly one VkInstance (created in onInit()) and, lazily, one
    // shared VkDevice (created the first time ANY window attaches -- see
    // attachToWindow() for why device creation can't happen any
    // earlier). Every window that attaches afterward shares that same
    // device, graphics queue, and present queue.
    //
    // This module has no opinion on rendering: no render passes, no
    // pipelines, no command pools, no idea what a "frame" is made of. It
    // only solves the boilerplate that's genuinely identical for every
    // consumer regardless of what they draw -- instance/device/surface/
    // swapchain lifecycle, and the per-frame acquire/present
    // synchronization that Vulkan requires -- so that a GUI renderer and
    // a game renderer (or several of each, in the same window) can each
    // build their own pipelines on top of the SAME device and share a
    // window's swapchain image by recording into it in whatever order
    // the application chooses. nativeInstance()/nativeDevice()/etc. are
    // the escape hatch into raw Vulkan for everything past that point --
    // unlike dsbox::WindowHandle, native Vulkan handles are not hidden
    // here on purpose: giving a renderer full, unmediated Vulkan access
    // is the entire point of this module.
    //
    // attachToWindow() takes a dsbox::IWindowProvider& rather than a
    // concrete dsbox_sdl::WindowModule&: this module's public API has
    // no dependency on dsbox_sdl (or any other specific windowing
    // module) at all, only on the core contract in
    // <dsbox/window_provider.hpp>. In practice the provider passed in
    // today is almost always a dsbox_sdl::WindowModule -- and, since
    // this module's .cpp already talks to SDL directly for
    // SDL_Vulkan_CreateSurface(), the native pointer handed back from
    // withNativeWindow() is still expected to be an SDL_Window* -- but
    // that expectation lives in the .cpp, not in this header, so a
    // future non-SDL IWindowProvider implementation is free to attach
    // here too as long as it hands back the native window type this
    // module's .cpp knows how to consume.
    //
    // A window must have been created with SDL's Vulkan-capable graphics
    // hint (dsbox_sdl::WindowGraphicsHint::Vulkan, see dsbox_sdl's
    // WindowOptions) before attachToWindow() can succeed for it -- SDL
    // enforces the corresponding native flag only at window-creation
    // time.
    //
    // Thread safety: onInit()/onShutdown() must be called like any other
    // IModule lifecycle callback (i.e. not concurrently with anything
    // else on this instance). attachToWindow()/detach()/
    // recreateSwapchain()/acquireNextImage() are safe to call
    // concurrently for DIFFERENT surfaces -- each surface's own state is
    // independently locked, and acquireNextImage()'s wait on the GPU
    // does not hold that lock (see the .cpp for exactly what's held
    // when). submit()/present() are ALSO safe to call concurrently for
    // different surfaces, but NOT for the reason the other methods
    // are: every surface shares the SAME graphicsQueue/presentQueue
    // (see above), and Vulkan requires external synchronization for any
    // concurrent use of one VkQueue -- submit()/present() provide that
    // synchronization internally (an internal mutex scoped to just the
    // vkQueueSubmit/vkQueuePresentKHR call itself), so callers don't
    // have to coordinate across surfaces themselves. Calling more than
    // one of any of these methods concurrently for the SAME SurfaceHandle
    // is still not supported -- matches how a single swapchain is meant
    // to be driven by exactly one frame loop.
    class DSBOX_VULKAN_API VulkanModule final : public dsbox::IModule
    {
    public:
        VulkanModule();
        ~VulkanModule() override;

        VulkanModule(const VulkanModule&) = delete;
        VulkanModule& operator=(const VulkanModule&) = delete;

        std::string_view name() const override
        {
            return "dsbox.vulkan";
        }

        void onInit(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Creates a VkSurfaceKHR for `handle`'s window (via
        // windowProvider.withNativeWindow() -- see dsbox::IWindowProvider)
        // and a swapchain sized to that window's current pixel size. The
        // very first successful call to this method across the module's
        // lifetime also creates the shared VkDevice: picking a physical
        // device and queue family properly requires a real VkSurfaceKHR
        // to test present support against
        // (vkGetPhysicalDeviceSurfaceSupportKHR), which is why device
        // creation can't happen back in onInit() before any window
        // exists.
        //
        // `windowProvider` is typically a dsbox_sdl::WindowModule (see
        // the class comment above for why this takes the core
        // dsbox::IWindowProvider contract instead of that concrete
        // type), and `handle` one of its dsbox_sdl::WindowHandle values
        // -- which is itself just an alias of dsbox::WindowHandle, so no
        // conversion is needed at the call site.
        //
        // Returns kInvalidSurfaceHandle (and records a lastError()) on
        // failure: no Vulkan support, the window wasn't created with
        // WindowGraphicsHint::Vulkan, no suitable physical device, etc.
        SurfaceHandle attachToWindow(dsbox::IWindowProvider& windowProvider, dsbox::WindowHandle handle);

        // Destroys `surface`'s swapchain and VkSurfaceKHR (waits for the
        // device to go idle first, so this is not a hot-path call).
        // Safe no-op if `surface` is unknown. Does not touch the shared
        // VkDevice -- that stays alive for as long as VulkanModule does,
        // even after the last surface detaches.
        void detach(SurfaceHandle surface);

        // Rebuilds `surface`'s swapchain in place (same VkSurfaceKHR) at
        // the given pixel size -- call this in response to a
        // "window.resized" event from dsbox_sdl, or whenever
        // acquireNextImage()/present() reports AcquiredFrame::outOfDate.
        // No-op if `surface` is unknown.
        void recreateSwapchain(SurfaceHandle surface, std::uint32_t width, std::uint32_t height);

        // Blocks until the next image is available, or the swapchain
        // turns out to be out of date (see AcquiredFrame::outOfDate) --
        // both are normal, expected outcomes, not necessarily errors.
        AcquiredFrame acquireNextImage(SurfaceHandle surface);

        // Presents the frame previously returned by acquireNextImage().
        // Returns false if the swapchain needs to be recreated (again,
        // an expected outcome mid-resize, not necessarily an error) --
        // call recreateSwapchain() and move on to the next frame.
        bool present(SurfaceHandle surface, const AcquiredFrame& frame);

        // Thin wrapper around vkQueueSubmit() on this module's shared
        // graphicsQueue(), synchronized internally against present() and
        // every other thread's own submit() call -- see this class's
        // own thread-safety note above for why that's required, not
        // optional, once more than one window/thread is involved. Prefer
        // this over calling vkQueueSubmit() directly on graphicsQueue()
        // unless you have a specific reason to manage that
        // synchronization yourself. `submitInfo` is passed through
        // as-is (command buffers, wait/signal semaphores, ... -- this
        // module has no opinion on any of it); `fence`, if not
        // VK_NULL_HANDLE, is typically an AcquiredFrame's own
        // frameCompleteFence. Returns false (with lastError() set) on
        // any Vulkan failure, or if no device/queue exists yet (nothing
        // has attachToWindow()'d successfully).
        bool submit(const VkSubmitInfo& submitInfo, VkFence fence = VK_NULL_HANDLE);

        // {} (all-zero/undefined) if `surface` is unknown.
        SwapchainInfo swapchainInfo(SurfaceHandle surface) const;

        std::string lastError() const;

        // Escape hatch into raw Vulkan for everything this module
        // doesn't do (render passes, pipelines, buffers, command
        // pools, ...). nativeInstance() is valid after a successful
        // onInit(); the rest are VK_NULL_HANDLE/0 until the first
        // attachToWindow() call has created the shared device.
        // graphicsQueue()/presentQueue() are shared across every
        // attached surface (see this class's own doc) -- prefer
        // submit()/present() above over calling vkQueueSubmit()/
        // vkQueuePresentKHR() on these directly; if you do call them
        // directly, YOU are responsible for the external synchronization
        // Vulkan requires against every other thread that might also
        // submit to or present on the same queue (including this
        // module's own submit()/present() calls for other surfaces).
        VkInstance nativeInstance() const noexcept;
        VkPhysicalDevice nativePhysicalDevice() const noexcept;
        VkDevice nativeDevice() const noexcept;
        std::uint32_t graphicsQueueFamily() const noexcept;
        VkQueue graphicsQueue() const noexcept;
        std::uint32_t presentQueueFamily() const noexcept;
        VkQueue presentQueue() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
