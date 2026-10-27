find_package(Vulkan REQUIRED)

add_library(DSBoxVulkan SHARED src/vulkan_module.cpp)
add_library(DSBox::Vulkan ALIAS DSBoxVulkan)

target_include_directories(DSBoxVulkan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxVulkan PRIVATE DSBOX_VULKAN_EXPORTING)

# Core is a public dependency: IModule/Context/IWindowProvider are part
# of DSBoxVulkan's own public API, same as every other module.
#
# Vulkan is public for a similar reason: VulkanModule's whole point is
# to hand out raw Vulkan types (VkInstance, VkDevice, VkImage, ...), so
# callers need <vulkan/vulkan.h> regardless.
#
# Note there is deliberately NO DSBox::SDL here, public or private:
# attachToWindow() takes a dsbox::IWindowProvider& (see
# <dsbox/window_provider.hpp>), not a concrete dsbox_sdl::WindowModule&,
# so DSBoxVulkan has no dependency on dsbox_sdl at all -- it works with
# ANY IWindowProvider implementation. SDL3 itself (as opposed to
# dsbox_sdl) is still needed as a PRIVATE implementation detail, since
# this module's .cpp assumes whatever native pointer it's handed back
# is an SDL_Window* (see attachToWindow()'s comment in the .cpp) and
# calls SDL_Vulkan_CreateSurface()/SDL_Vulkan_GetInstanceExtensions()
# directly -- neither appears in a public header.
#
# Config is also a private implementation detail, only touched inside
# onInit() to read "[vulkan]" settings.
target_link_libraries(DSBoxVulkan
    PUBLIC
        DSBox::Core
        Vulkan::Vulkan
    PRIVATE
        DSBox::Config
        SDL3::SDL3
)

if(NOT MSVC)
    target_link_libraries(DSBoxVulkan PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
