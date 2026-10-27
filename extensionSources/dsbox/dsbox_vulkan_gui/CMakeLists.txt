find_package(Vulkan REQUIRED)

include(FetchContent)

# stb_truetype.h -- single-header TrueType parsing/rasterization, used
# ONLY inside vulkan_ui_renderer.cpp (never exposed through any public
# header) to bake a glyph atlas for dsbox_ui::Text. Pinned to the
# commit that shipped stb_truetype 1.26 rather than "master", so a
# from-scratch build stays reproducible -- bump this deliberately, not
# as a side effect of an unrelated change. Only the include directory
# is used; nothing from stb itself is compiled or linked.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG c0c982601f40183e74d84a61237e968dca08380e
)
FetchContent_MakeAvailable(stb)

add_library(DSBoxVulkanGui SHARED
    src/vulkan_ui_renderer.cpp
    src/vulkan_ui_window_driver.cpp
)
add_library(DSBox::VulkanGui ALIAS DSBoxVulkanGui)

target_include_directories(DSBoxVulkanGui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# PRIVATE on purpose: stb_truetype.h must never leak into a public
# header of this module (see vulkan_ui_renderer.hpp's LoadedFontImpl).
target_include_directories(DSBoxVulkanGui PRIVATE ${stb_SOURCE_DIR})

target_compile_definitions(DSBoxVulkanGui PRIVATE DSBOX_VULKAN_GUI_EXPORTING)

# DSBox::Ui is public: IUiRenderBackend/DisplayList/UiRect are part of
# VulkanUiRenderer's own public API. DSBox::Vulkan is public too --
# VulkanUiRenderer's constructor and renderFrame() both take
# dsbox_vulkan types directly (VulkanModule&, AcquiredFrame). Note
# there is deliberately NO DSBox::SDL here, public or private: this
# module never touches windowing directly, only through what
# dsbox_vulkan/dsbox_ui already hand it.
target_link_libraries(DSBoxVulkanGui
    PUBLIC
        DSBox::Core
        DSBox::Ui
        DSBox::Vulkan
        Vulkan::Vulkan
)

if(NOT MSVC)
    target_link_libraries(DSBoxVulkanGui PRIVATE Threads::Threads)
endif()

# -- Shader compilation ---------------------------------------------
#
# shaders/rect.{vert,frag} are compiled to SPIR-V by glslc at build
# time, then copied into the fin-out/ output root's own top-level
# shaders/dsbox_vulkan_gui/ folder (a sibling of fin-out/bin/, not
# nested under the built binary's directory). VulkanUiRenderer's
# constructor takes that folder's path explicitly (see its own header
# comment) rather than hardcoding it -- this copy step just gives
# local development a sensible default to point at (e.g. resolved
# relative to the host application's own executable path, two levels
# up from fin-out/bin/executables/ or fin-out/bin/framework/<x>/).
if(TARGET Vulkan::glslc)
    get_target_property(DSBOX_VULKAN_GUI_GLSLC Vulkan::glslc IMPORTED_LOCATION)
endif()
if(NOT DSBOX_VULKAN_GUI_GLSLC)
    find_program(DSBOX_VULKAN_GUI_GLSLC glslc REQUIRED)
endif()

set(DSBOX_VULKAN_GUI_SPV_FILES "")

foreach(shader IN ITEMS rect.vert rect.frag)
    set(shader_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(shader_out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.spv")

    add_custom_command(
        OUTPUT "${shader_out}"
        COMMAND "${DSBOX_VULKAN_GUI_GLSLC}" "${shader_src}" -o "${shader_out}"
        DEPENDS "${shader_src}"
        COMMENT "Compiling dsbox_vulkan_gui/${shader} to SPIR-V"
        VERBATIM
    )

    list(APPEND DSBOX_VULKAN_GUI_SPV_FILES "${shader_out}")
endforeach()

add_custom_target(DSBoxVulkanGuiShaders DEPENDS ${DSBOX_VULKAN_GUI_SPV_FILES})
add_dependencies(DSBoxVulkanGui DSBoxVulkanGuiShaders)

add_custom_command(
    TARGET DSBoxVulkanGui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${DSBOX_OUTPUT_ROOT}/shaders/dsbox_vulkan_gui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${DSBOX_VULKAN_GUI_SPV_FILES}
            "${DSBOX_OUTPUT_ROOT}/shaders/dsbox_vulkan_gui"
    COMMENT "Copying dsbox_vulkan_gui shaders into fin-out/shaders"
)

# No tests/ subdirectory yet -- a meaningful automated test here would
# need either a real GPU + golden-image comparison or a from-scratch
# software rasterizer to check against, neither of which exists in
# this codebase yet. Left as a follow-up rather than a token smoke
# test that wouldn't actually catch a rendering regression.
