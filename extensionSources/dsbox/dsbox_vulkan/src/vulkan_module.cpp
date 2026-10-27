#include <dsbox_vulkan/vulkan_module.hpp>

#include <dsbox/context.hpp>
#include <dsbox/static_registry.hpp>

#include <dsbox_config/persistent_config_store.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace
{
    std::string toLower(std::string_view text)
    {
        std::string out(text);
        std::transform(
            out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    // Reads a "[vulkan]" section from Context's PersistentConfigStore,
    // if one is reachable, falling back to VulkanOptions{}'s hardcoded
    // defaults for anything absent or malformed. This is the ONLY place
    // dsbox_vulkan knows dsbox_config exists -- kept private to the .cpp
    // so the public header (and everyone who only wants VulkanOptions
    // itself) never has to depend on it.
    dsbox_vulkan::VulkanOptions loadOptionsFromConfig(dsbox::Context& context)
    {
        dsbox_vulkan::VulkanOptions options;

        auto config = context.get<dsbox_config::PersistentConfigStore>();
        if (!config)
            return options;

        options.enableValidation = config->getBool("vulkan", "validation", options.enableValidation);
        options.preferredDeviceName = config->getOr("vulkan", "device_name", options.preferredDeviceName);
        options.presentMode = config->getOr("vulkan", "present_mode", options.presentMode);

        std::string indexText = config->getOr("vulkan", "device_index", std::string());
        if (!indexText.empty())
        {
            try
            {
                options.preferredDeviceIndex = std::stoi(indexText);
            }
            catch (...)
            {
                // Malformed value in the config file -- keep the "no
                // preference" default rather than fail startup over it.
            }
        }

        std::string framesText = config->getOr("vulkan", "frames_in_flight", std::string());
        if (!framesText.empty())
        {
            try
            {
                int parsed = std::stoi(framesText);
                if (parsed >= 1)
                    options.framesInFlight = static_cast<std::uint32_t>(parsed);
            }
            catch (...)
            {
                // Keep the default.
            }
        }

        return options;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT /*type*/,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* /*userData*/)
    {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && data)
            std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);

        return VK_FALSE;
    }

    bool hasValidationLayer()
    {
        std::uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);

        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());

        for (const auto& layer : layers)
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
                return true;

        return false;
    }

    struct QueueFamilyIndices
    {
        std::optional<std::uint32_t> graphics;
        std::optional<std::uint32_t> present;

        bool complete() const
        {
            return graphics.has_value() && present.has_value();
        }
    };

    // Prefers a single queue family that does both graphics and present
    // (the common case on desktop GPUs) but falls back to two separate
    // families if that's what the device actually offers.
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface)
    {
        QueueFamilyIndices result;

        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);

        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

        for (std::uint32_t i = 0; i < count; ++i)
        {
            bool graphicsCapable = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;

            if (graphicsCapable && !result.graphics.has_value())
                result.graphics = i;

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

            if (presentSupport && (!result.present.has_value() || graphicsCapable))
                result.present = i; // prefer a family that also does graphics, if we find one
        }

        return result;
    }

    bool deviceSupportsSwapchainExtension(VkPhysicalDevice device)
    {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());

        for (const auto& extension : extensions)
            if (std::strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
                return true;

        return false;
    }

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
    {
        for (const auto& candidate : formats)
            if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB &&
                candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return candidate;

        return formats.front();
    }

    VkPresentModeKHR choosePresentMode(
        const std::vector<VkPresentModeKHR>& available, const std::string& requested)
    {
        VkPresentModeKHR wanted = VK_PRESENT_MODE_FIFO_KHR;

        if (requested == "mailbox")
            wanted = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (requested == "immediate")
            wanted = VK_PRESENT_MODE_IMMEDIATE_KHR;

        for (auto mode : available)
            if (mode == wanted)
                return mode;

        return VK_PRESENT_MODE_FIFO_KHR; // guaranteed available by the spec
    }

    VkExtent2D chooseExtent(
        const VkSurfaceCapabilitiesKHR& capabilities, std::uint32_t width, std::uint32_t height)
    {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
            return capabilities.currentExtent;

        VkExtent2D extent{ width, height };
        extent.width = std::clamp(
            extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(
            extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return extent;
    }
}

namespace dsbox_vulkan
{
    // Everything needed to keep one window's swapchain alive and to
    // drive its per-frame acquire/present synchronization. See the
    // "Frames in flight" pattern this mirrors: imageAvailable/
    // renderFinished/inFlightFence are indexed by [currentFrame] (a
    // rotating slot, framesInFlight of them); imagesInFlight is
    // indexed by [swapchain image index] and lets acquireNextImage()
    // detect "this specific image is still being used by an OLDER
    // in-flight frame" even when imageCount != framesInFlight.
    struct VulkanModule::Impl
    {
        struct SurfaceState
        {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkExtent2D extent{};

            std::vector<VkImage> images;
            std::vector<VkImageView> imageViews;

            std::vector<VkSemaphore> imageAvailableSemaphores;
            std::vector<VkSemaphore> renderFinishedSemaphores;
            std::vector<VkFence> inFlightFences;
            std::vector<VkFence> imagesInFlight; // aliases into inFlightFences, or VK_NULL_HANDLE

            std::uint32_t currentFrame = 0;
        };

        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
        bool validationEnabled = false;

        // SDL's video subsystem and Vulkan loader are reference-counted
        // by SDL itself, so it's safe for VulkanModule to ensure both
        // are initialized on its own in onInit() (in case it runs before
        // -- or entirely without -- dsbox_sdl's WindowModule) and to
        // release its own reference in onShutdown(), without needing a
        // hard IModule dependency on dsbox_sdl.
        bool ownsVideoSubsystem = false;
        bool vulkanLibraryLoaded = false;

        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        std::uint32_t graphicsQueueFamily = 0;
        std::uint32_t presentQueueFamily = 0;
        VkQueue graphicsQueue = VK_NULL_HANDLE;
        VkQueue presentQueue = VK_NULL_HANDLE;

        VulkanOptions options;

        // Guards `surfaces` and the lazy `device` creation. NOT held
        // during acquireNextImage()'s wait on the GPU (vkWaitForFences/
        // vkAcquireNextImageKHR) -- only while looking up/updating the
        // SurfaceState entry itself -- so two different surfaces can
        // block on the GPU concurrently without blocking each other.
        mutable std::mutex mutex;
        std::unordered_map<SurfaceHandle, SurfaceState> surfaces;
        std::atomic<std::uint64_t> nextHandle{ 1 };

        // Guards vkQueueSubmit()/vkQueuePresentKHR() specifically --
        // separate from `mutex` above on purpose (queue submission
        // shouldn't have to wait on unrelated surface-map bookkeeping,
        // or vice versa). ALL surfaces share one graphicsQueue/
        // presentQueue (see this class's own doc), and Vulkan requires
        // external synchronization for concurrent use of the SAME
        // VkQueue from multiple threads -- without this, two windows
        // each driven by their own thread WILL eventually submit/present
        // concurrently and corrupt driver-internal state (this is not
        // hypothetical -- see submit()'s own comment for how this was
        // found). Held only around the vkQueueSubmit/vkQueuePresentKHR
        // calls themselves, never across a GPU wait.
        mutable std::mutex queueMutex;

        mutable std::mutex errorMutex;
        std::string lastError;

        void setError(std::string message);

        // Shared by ~VulkanModule() and onShutdown() -- see its own
        // comment below for why they can't just call each other
        // directly.
        void teardown() noexcept;

        bool createInstance(const std::vector<const char*>& extraLayers);

        // Physical device + logical device + queues, created exactly
        // once, the first time attachToWindow() is called with no
        // existing device. `probeSurface` is used only to test present
        // support during selection; it is not retained.
        bool ensureDeviceCreated(VkSurfaceKHR probeSurface);

        // Builds (or, if `state.swapchain` is already set, rebuilds in
        // place) the swapchain, images, and image views for `state`.
        // Sync objects are created separately (see createSyncObjects())
        // since they don't need to be recreated on resize.
        bool createSwapchainFor(SurfaceState& state, std::uint32_t width, std::uint32_t height);

        void destroySwapchainResources(SurfaceState& state);
        void createSyncObjects(SurfaceState& state);
        void destroySyncObjects(SurfaceState& state);
    };

    void VulkanModule::Impl::teardown() noexcept
    {
        // Shared by ~VulkanModule() (in case onShutdown() was skipped by
        // abnormal teardown) and onShutdown() itself. Every member is
        // explicitly reset to its "never initialized" value at the end,
        // so this object stays safely reusable afterward --
        // ModuleManager::reload() re-runs onInit() on the SAME instance
        // (see module_manager.hpp), and onShutdown() must leave it valid
        // for that, not merely torn down.
        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);

            for (auto& [handle, state] : surfaces)
            {
                destroySyncObjects(state);
                destroySwapchainResources(state);
                if (state.surface != VK_NULL_HANDLE)
                    vkDestroySurfaceKHR(instance, state.surface, nullptr);
            }
            surfaces.clear();

            vkDestroyDevice(device, nullptr);
        }

        if (debugMessenger != VK_NULL_HANDLE)
        {
            auto destroyFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));

            if (destroyFn)
                destroyFn(instance, debugMessenger, nullptr);
        }

        if (instance != VK_NULL_HANDLE)
            vkDestroyInstance(instance, nullptr);

        if (vulkanLibraryLoaded)
            SDL_Vulkan_UnloadLibrary();

        if (ownsVideoSubsystem)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);

        instance = VK_NULL_HANDLE;
        debugMessenger = VK_NULL_HANDLE;
        validationEnabled = false;
        ownsVideoSubsystem = false;
        vulkanLibraryLoaded = false;
        physicalDevice = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        graphicsQueueFamily = 0;
        presentQueueFamily = 0;
        graphicsQueue = VK_NULL_HANDLE;
        presentQueue = VK_NULL_HANDLE;
        options = VulkanOptions{};
        nextHandle.store(1);
        setError(std::string());
    }

    void VulkanModule::Impl::setError(std::string message)
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = std::move(message);
    }

    bool VulkanModule::Impl::createInstance(const std::vector<const char*>& extraLayers)
    {
        std::uint32_t sdlExtensionCount = 0;
        char const* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);

        if (!sdlExtensions)
        {
            setError(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
            return false;
        }

        std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);

        if (validationEnabled)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "DSBox";
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        appInfo.pEngineName = "DSBox";
        appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();
        createInfo.enabledLayerCount = static_cast<std::uint32_t>(extraLayers.size());
        createInfo.ppEnabledLayerNames = extraLayers.empty() ? nullptr : extraLayers.data();

        if (validationEnabled)
            createInfo.pNext = &debugCreateInfo; // also validates instance create/destroy themselves

        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);

        if (result != VK_SUCCESS)
        {
            setError("vkCreateInstance failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            return false;
        }

        if (validationEnabled)
        {
            auto createFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));

            if (createFn)
                createFn(instance, &debugCreateInfo, nullptr, &debugMessenger);
            // If createFn is somehow null despite the layer being
            // present, we just run without the messenger -- not worth
            // failing instance creation over a diagnostics feature.
        }

        return true;
    }

    bool VulkanModule::Impl::ensureDeviceCreated(VkSurfaceKHR probeSurface)
    {
        if (device != VK_NULL_HANDLE)
            return true; // already created by an earlier attachToWindow()

        std::uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

        if (deviceCount == 0)
        {
            setError("No Vulkan-capable physical devices found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        struct Candidate
        {
            VkPhysicalDevice device;
            QueueFamilyIndices queues;
            VkPhysicalDeviceProperties properties;
        };

        std::vector<Candidate> candidates;

        for (auto candidateDevice : devices)
        {
            if (!deviceSupportsSwapchainExtension(candidateDevice))
                continue;

            QueueFamilyIndices queues = findQueueFamilies(candidateDevice, probeSurface);
            if (!queues.complete())
                continue;

            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidateDevice, &properties);

            candidates.push_back(Candidate{ candidateDevice, queues, properties });
        }

        if (candidates.empty())
        {
            setError("No physical device supports both VK_KHR_swapchain and "
                      "presenting to this window's surface");
            return false;
        }

        const Candidate* chosen = nullptr;

        if (options.preferredDeviceIndex >= 0 &&
            static_cast<std::size_t>(options.preferredDeviceIndex) < candidates.size())
        {
            chosen = &candidates[static_cast<std::size_t>(options.preferredDeviceIndex)];
        }
        else if (!options.preferredDeviceName.empty())
        {
            std::string wanted = toLower(options.preferredDeviceName);

            for (const auto& candidate : candidates)
            {
                if (toLower(candidate.properties.deviceName).find(wanted) != std::string::npos)
                {
                    chosen = &candidate;
                    break;
                }
            }
        }

        if (!chosen)
        {
            for (const auto& candidate : candidates)
            {
                if (candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                {
                    chosen = &candidate;
                    break;
                }
            }
        }

        if (!chosen)
            chosen = &candidates.front();

        std::vector<std::uint32_t> uniqueFamilies{ *chosen->queues.graphics };
        if (*chosen->queues.present != *chosen->queues.graphics)
            uniqueFamilies.push_back(*chosen->queues.present);

        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

        for (auto family : uniqueFamilies)
        {
            VkDeviceQueueCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            info.queueFamilyIndex = family;
            info.queueCount = 1;
            info.pQueuePriorities = &priority;
            queueCreateInfos.push_back(info);
        }

        const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkPhysicalDeviceFeatures features{}; // no optional features required yet

        VkDeviceCreateInfo deviceCreateInfo{};
        deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceCreateInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
        deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
        deviceCreateInfo.pEnabledFeatures = &features;
        deviceCreateInfo.enabledExtensionCount = 1;
        deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions;

        VkResult result = vkCreateDevice(chosen->device, &deviceCreateInfo, nullptr, &device);

        if (result != VK_SUCCESS)
        {
            setError("vkCreateDevice failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            return false;
        }

        physicalDevice = chosen->device;
        graphicsQueueFamily = *chosen->queues.graphics;
        presentQueueFamily = *chosen->queues.present;

        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
        vkGetDeviceQueue(device, presentQueueFamily, 0, &presentQueue);

        return true;
    }

    bool VulkanModule::Impl::createSwapchainFor(SurfaceState& state, std::uint32_t width, std::uint32_t height)
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, state.surface, &capabilities);

        std::uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, state.surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, state.surface, &formatCount, formats.data());

        if (formats.empty())
        {
            setError("Surface reports no supported formats");
            return false;
        }

        std::uint32_t presentModeCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, state.surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice, state.surface, &presentModeCount, presentModes.data());

        VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
        VkPresentModeKHR presentMode = choosePresentMode(presentModes, options.presentMode);
        VkExtent2D extent = chooseExtent(capabilities, width, height);

        std::uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
            imageCount = capabilities.maxImageCount;

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = state.surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        std::uint32_t familyIndices[] = { graphicsQueueFamily, presentQueueFamily };

        if (graphicsQueueFamily != presentQueueFamily)
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = familyIndices;
        }
        else
        {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = state.swapchain; // may be VK_NULL_HANDLE (first creation)

        VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
        VkResult result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &newSwapchain);

        if (result != VK_SUCCESS)
        {
            setError("vkCreateSwapchainKHR failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            return false;
        }

        // Safe to tear down the OLD swapchain's images/views now that the
        // new one exists -- passing it as oldSwapchain above let the
        // driver hand over the surface without a gap. Callers that might
        // still be mid-frame against the old swapchain are the reason
        // recreateSwapchain() waits on this surface's own inFlightFences
        // first (see its own comment) before ever reaching this function.
        destroySwapchainResources(state);

        state.swapchain = newSwapchain;
        state.format = surfaceFormat.format;
        state.extent = extent;

        std::uint32_t actualImageCount = 0;
        vkGetSwapchainImagesKHR(device, state.swapchain, &actualImageCount, nullptr);
        state.images.resize(actualImageCount);
        vkGetSwapchainImagesKHR(device, state.swapchain, &actualImageCount, state.images.data());

        state.imageViews.assign(actualImageCount, VK_NULL_HANDLE);

        for (std::size_t i = 0; i < actualImageCount; ++i)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = state.images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = state.format;
            viewInfo.components = {
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
            };
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device, &viewInfo, nullptr, &state.imageViews[i]) != VK_SUCCESS)
            {
                setError("vkCreateImageView failed for swapchain image " + std::to_string(i));
                return false;
            }
        }

        // Per-image "who last used this image" tracking (see
        // acquireNextImage()) -- these are brand new VkImage objects
        // even when the count happens to match the previous swapchain's,
        // so there is nothing valid to carry over.
        state.imagesInFlight.assign(actualImageCount, VK_NULL_HANDLE);

        return true;
    }

    void VulkanModule::Impl::destroySwapchainResources(SurfaceState& state)
    {
        for (auto view : state.imageViews)
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device, view, nullptr);

        state.imageViews.clear();
        state.images.clear();

        if (state.swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(device, state.swapchain, nullptr);
            state.swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanModule::Impl::createSyncObjects(SurfaceState& state)
    {
        state.imageAvailableSemaphores.assign(options.framesInFlight, VK_NULL_HANDLE);
        state.renderFinishedSemaphores.assign(options.framesInFlight, VK_NULL_HANDLE);
        state.inFlightFences.assign(options.framesInFlight, VK_NULL_HANDLE);

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first acquire's wait doesn't block forever

        for (std::uint32_t i = 0; i < options.framesInFlight; ++i)
        {
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &state.imageAvailableSemaphores[i]);
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &state.renderFinishedSemaphores[i]);
            vkCreateFence(device, &fenceInfo, nullptr, &state.inFlightFences[i]);
        }

        state.currentFrame = 0;
    }

    void VulkanModule::Impl::destroySyncObjects(SurfaceState& state)
    {
        for (auto semaphore : state.imageAvailableSemaphores)
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device, semaphore, nullptr);

        for (auto semaphore : state.renderFinishedSemaphores)
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(device, semaphore, nullptr);

        for (auto fence : state.inFlightFences)
            if (fence != VK_NULL_HANDLE)
                vkDestroyFence(device, fence, nullptr);

        state.imageAvailableSemaphores.clear();
        state.renderFinishedSemaphores.clear();
        state.inFlightFences.clear();
        state.imagesInFlight.clear();
    }

    VulkanModule::VulkanModule() : m_impl(std::make_unique<Impl>())
    {
    }

    VulkanModule::~VulkanModule()
    {
        m_impl->teardown();
    }

    std::string VulkanModule::lastError() const
    {
        std::lock_guard<std::mutex> lock(m_impl->errorMutex);
        return m_impl->lastError;
    }

    VkInstance VulkanModule::nativeInstance() const noexcept
    {
        return m_impl->instance;
    }

    VkPhysicalDevice VulkanModule::nativePhysicalDevice() const noexcept
    {
        return m_impl->physicalDevice;
    }

    VkDevice VulkanModule::nativeDevice() const noexcept
    {
        return m_impl->device;
    }

    std::uint32_t VulkanModule::graphicsQueueFamily() const noexcept
    {
        return m_impl->graphicsQueueFamily;
    }

    VkQueue VulkanModule::graphicsQueue() const noexcept
    {
        return m_impl->graphicsQueue;
    }

    std::uint32_t VulkanModule::presentQueueFamily() const noexcept
    {
        return m_impl->presentQueueFamily;
    }

    VkQueue VulkanModule::presentQueue() const noexcept
    {
        return m_impl->presentQueue;
    }

    void VulkanModule::onInit(dsbox::Context& context)
    {
        m_impl->options = loadOptionsFromConfig(context);

        // SDL reference-counts both the video subsystem and the Vulkan
        // loader, so it's harmless if dsbox_sdl's WindowModule (or
        // anything else) already initialized either -- see the member
        // comments in Impl for why this module doesn't require a hard
        // dependency on dsbox_sdl to do this itself.
        m_impl->ownsVideoSubsystem = SDL_InitSubSystem(SDL_INIT_VIDEO);
        if (!m_impl->ownsVideoSubsystem)
        {
            m_impl->setError(std::string("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: ") + SDL_GetError());
            return;
        }

        if (!SDL_Vulkan_LoadLibrary(nullptr))
        {
            m_impl->setError(std::string("SDL_Vulkan_LoadLibrary failed: ") + SDL_GetError());
            return;
        }
        m_impl->vulkanLibraryLoaded = true;

        m_impl->validationEnabled = m_impl->options.enableValidation && hasValidationLayer();

        if (m_impl->options.enableValidation && !m_impl->validationEnabled)
        {
            // Not fatal -- keep going without it. If nothing else fails,
            // this note is the only thing lastError() will report, which
            // is the point: it's a diagnostic trail, not strictly "why
            // onInit failed".
            m_impl->setError("Validation requested via config but VK_LAYER_KHRONOS_validation "
                      "is not available on this machine -- continuing without it");
        }

        std::vector<const char*> extraLayers;
        if (m_impl->validationEnabled)
            extraLayers.push_back("VK_LAYER_KHRONOS_validation");

        m_impl->createInstance(extraLayers); // failure leaves instance null; lastError() already set
    }

    void VulkanModule::onShutdown(dsbox::Context&)
    {
        m_impl->teardown();
    }

    SurfaceHandle VulkanModule::attachToWindow(
        dsbox::IWindowProvider& windowProvider, dsbox::WindowHandle handle)
    {
        if (m_impl->instance == VK_NULL_HANDLE)
        {
            m_impl->setError("VulkanModule::onInit() did not complete successfully; see lastError()");
            return kInvalidSurfaceHandle;
        }

        VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
        int pixelWidth = 0;
        int pixelHeight = 0;
        bool surfaceCreated = false;

        Impl* impl = m_impl.get();

        // `windowProvider` only promises a native pointer via the
        // dsbox::IWindowProvider contract (see window_provider.hpp) --
        // it says nothing about what kind of pointer that is. This
        // module works specifically with SDL-backed providers (e.g.
        // dsbox_sdl::WindowModule), so the cast back to SDL_Window* here
        // is this module's own, deliberate assumption about what it was
        // handed, not something the contract itself guarantees.
        windowProvider.withNativeWindow(
            handle,
            [&](void* nativeWindow)
            {
                SDL_Window* window = static_cast<SDL_Window*>(nativeWindow);
                surfaceCreated = SDL_Vulkan_CreateSurface(window, impl->instance, nullptr, &rawSurface);
                if (surfaceCreated)
                    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
            });

        if (!surfaceCreated)
        {
            impl->setError(
                std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError() +
                " (was the window created with dsbox_sdl::WindowGraphicsHint::Vulkan?)");
            return kInvalidSurfaceHandle;
        }

        std::lock_guard<std::mutex> lock(impl->mutex);

        if (!impl->ensureDeviceCreated(rawSurface))
        {
            vkDestroySurfaceKHR(impl->instance, rawSurface, nullptr);
            return kInvalidSurfaceHandle;
        }

        // Defensive check for a surface attached AFTER the device
        // already exists (created against an earlier window): confirm
        // the existing present queue family can still present to THIS
        // surface. Virtually always true on a single-GPU desktop system
        // -- a genuine multi-adapter mismatch is not supported here.
        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(
            impl->physicalDevice, impl->presentQueueFamily, rawSurface, &presentSupported);

        if (!presentSupported)
        {
            impl->setError("The existing Vulkan device's present queue cannot present to this "
                      "window's surface (likely a multi-GPU/multi-adapter setup, which this "
                      "version of dsbox_vulkan does not support)");
            vkDestroySurfaceKHR(impl->instance, rawSurface, nullptr);
            return kInvalidSurfaceHandle;
        }

        Impl::SurfaceState state;
        state.surface = rawSurface;

        if (!impl->createSwapchainFor(
                state, static_cast<std::uint32_t>(pixelWidth), static_cast<std::uint32_t>(pixelHeight)))
        {
            impl->destroySwapchainResources(state);
            vkDestroySurfaceKHR(impl->instance, rawSurface, nullptr);
            return kInvalidSurfaceHandle;
        }

        impl->createSyncObjects(state);

        SurfaceHandle surfaceHandle = impl->nextHandle.fetch_add(1);
        impl->surfaces.emplace(surfaceHandle, std::move(state));
        return surfaceHandle;
    }

    void VulkanModule::detach(SurfaceHandle surface)
    {
        Impl* impl = m_impl.get();
        std::lock_guard<std::mutex> lock(impl->mutex);

        auto it = impl->surfaces.find(surface);
        if (it == impl->surfaces.end())
            return;

        // Same reasoning as recreateSwapchain() (see its own comment):
        // `device` is shared across every attached window, so a
        // vkDeviceWaitIdle() here would stall every OTHER window for
        // the duration of closing THIS one. This surface's own
        // inFlightFences already track exactly which GPU work is still
        // using its images/command buffers -- waiting on just those is
        // enough to safely tear its resources down below.
        if (impl->device != VK_NULL_HANDLE && !it->second.inFlightFences.empty())
        {
            vkWaitForFences(
                impl->device,
                static_cast<std::uint32_t>(it->second.inFlightFences.size()),
                it->second.inFlightFences.data(),
                VK_TRUE, UINT64_MAX);
        }

        impl->destroySyncObjects(it->second);
        impl->destroySwapchainResources(it->second);

        if (it->second.surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(impl->instance, it->second.surface, nullptr);

        impl->surfaces.erase(it);
    }

    void VulkanModule::recreateSwapchain(SurfaceHandle surface, std::uint32_t width, std::uint32_t height)
    {
        Impl* impl = m_impl.get();
        std::lock_guard<std::mutex> lock(impl->mutex);

        auto it = impl->surfaces.find(surface);
        if (it == impl->surfaces.end())
            return;

        if (width == 0 || height == 0)
            return; // e.g. a minimized window -- nothing sensible to create; caller should retry later

        // The old swapchain's images might still be referenced by an
        // in-flight present -- see createSwapchainFor()'s comment on why
        // it's safe to destroy right after creating the replacement, as
        // long as we've actually waited for that in-flight work first.
        //
        // Deliberately NOT vkDeviceWaitIdle(): `device` is SHARED by
        // every attached window (see this class's own doc), so a
        // device-wide wait here would stall every OTHER window's render
        // thread for the duration of THIS window's resize -- and since
        // this runs once per onFrame() call for as long as a window is
        // actively being resized (see VulkanUiWindowDriver::onFrame()),
        // that's not a rare hiccup, it's continuous cross-window
        // stalling for as long as the user keeps dragging this one
        // window's edge. This surface's own inFlightFences already
        // track exactly which GPU work is still using ITS images (see
        // the "Frames in flight" comment on SurfaceState, and the same
        // fences' use in acquireNextImage()) -- waiting on just those is
        // sufficient to safely destroy this surface's old swapchain
        // resources below, and touches nothing belonging to any other
        // surface.
        if (!it->second.inFlightFences.empty())
        {
            vkWaitForFences(
                impl->device,
                static_cast<std::uint32_t>(it->second.inFlightFences.size()),
                it->second.inFlightFences.data(),
                VK_TRUE, UINT64_MAX);
        }

        impl->createSwapchainFor(it->second, width, height); // failure leaves lastError() set; surface stays registered
    }

    AcquiredFrame VulkanModule::acquireNextImage(SurfaceHandle surface)
    {
        AcquiredFrame frame;
        frame.surface = surface;

        Impl* impl = m_impl.get();
        Impl::SurfaceState* state = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);

            auto it = impl->surfaces.find(surface);
            if (it == impl->surfaces.end())
            {
                frame.outOfDate = true;
                return frame;
            }

            state = &it->second;
        }

        // impl->mutex is deliberately NOT held from here on: this talks
        // to the GPU (vkWaitForFences/vkAcquireNextImageKHR) and must not
        // block acquire/present calls for OTHER surfaces. Safe because
        // concurrent calls for the SAME surface are unsupported (see the
        // class comment) -- only one frame loop ever drives this
        // SurfaceState, so `state` can't be invalidated (e.g. by
        // detach()) out from under us in practice; a caller that detaches
        // a surface while its own frame loop is still using it is
        // misusing the API regardless.

        VkFence inFlightFence = state->inFlightFences[state->currentFrame];
        vkWaitForFences(impl->device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);

        VkSemaphore imageAvailable = state->imageAvailableSemaphores[state->currentFrame];

        std::uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(
            impl->device, state->swapchain, UINT64_MAX, imageAvailable, VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            // Deliberately simple: treat "suboptimal" the same as "out of
            // date" rather than presenting this once and recreating
            // after. Slightly more wasted frames on resize; much less
            // edge-case surface area.
            frame.outOfDate = true;
            return frame;
        }

        if (result != VK_SUCCESS)
        {
            impl->setError("vkAcquireNextImageKHR failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            frame.outOfDate = true;
            return frame;
        }

        if (state->imagesInFlight[imageIndex] != VK_NULL_HANDLE)
            vkWaitForFences(impl->device, 1, &state->imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);

        state->imagesInFlight[imageIndex] = inFlightFence;
        vkResetFences(impl->device, 1, &inFlightFence); // caller's submit will re-signal it

        frame.imageIndex = imageIndex;
        frame.image = state->images[imageIndex];
        frame.imageView = state->imageViews[imageIndex];
        frame.format = state->format;
        frame.extent = state->extent;
        frame.imageAvailable = imageAvailable;
        frame.renderFinished = state->renderFinishedSemaphores[state->currentFrame];
        frame.frameCompleteFence = inFlightFence;
        frame.outOfDate = false;

        return frame;
    }

    bool VulkanModule::submit(const VkSubmitInfo& submitInfo, VkFence fence)
    {
        Impl* impl = m_impl.get();

        if (impl->graphicsQueue == VK_NULL_HANDLE)
            return false;

        // Serializes with present() below (and with every other thread's
        // submit() call) -- see queueMutex's own comment for why this is
        // required, not merely cautious: this module hands the SAME
        // VkQueue to every attached surface, and Vulkan does not allow
        // concurrent use of one VkQueue from multiple threads without
        // external synchronization. A host application that calls
        // vkQueueSubmit() directly on graphicsQueue() instead of through
        // here takes over responsibility for that synchronization itself
        // -- see graphicsQueue()'s own doc.
        std::lock_guard<std::mutex> lock(impl->queueMutex);

        VkResult result = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, fence);

        if (result != VK_SUCCESS)
        {
            impl->setError("vkQueueSubmit failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            return false;
        }

        return true;
    }

    bool VulkanModule::present(SurfaceHandle surface, const AcquiredFrame& frame)
    {
        if (frame.outOfDate)
            return false;

        Impl* impl = m_impl.get();
        Impl::SurfaceState* state = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);

            auto it = impl->surfaces.find(surface);
            if (it == impl->surfaces.end())
                return false;

            state = &it->second;
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &frame.renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &state->swapchain;
        presentInfo.pImageIndices = &frame.imageIndex;

        VkResult result;
        {
            // See queueMutex's own comment and submit()'s: this serializes
            // with submit() (and every other thread's present()) for the
            // same reason -- presentQueue is shared across every attached
            // surface.
            std::lock_guard<std::mutex> lock(impl->queueMutex);
            result = vkQueuePresentKHR(impl->presentQueue, &presentInfo);
        }

        state->currentFrame =
            (state->currentFrame + 1) % static_cast<std::uint32_t>(state->inFlightFences.size());

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            return false;

        if (result != VK_SUCCESS)
        {
            impl->setError("vkQueuePresentKHR failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
            return false;
        }

        return true;
    }

    SwapchainInfo VulkanModule::swapchainInfo(SurfaceHandle surface) const
    {
        Impl* impl = m_impl.get();
        std::lock_guard<std::mutex> lock(impl->mutex);

        auto it = impl->surfaces.find(surface);
        if (it == impl->surfaces.end())
            return {};

        SwapchainInfo info;
        info.format = it->second.format;
        info.extent = it->second.extent;
        info.imageCount = static_cast<std::uint32_t>(it->second.images.size());
        return info;
    }
}

DSBOX_DYNAMIC_MODULE(dsbox_vulkan::VulkanModule)
