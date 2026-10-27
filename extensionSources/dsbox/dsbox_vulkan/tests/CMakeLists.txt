add_executable(DSBoxVulkanTests
    main.cpp
    vulkan_module_tests.cpp
)

target_link_libraries(DSBoxVulkanTests PRIVATE
    DSBox::Vulkan
    DSBox::SDL
    DSBox::Config
    Vulkan::Vulkan
    SDL3::SDL3
)

if(NOT MSVC)
    target_link_libraries(DSBoxVulkanTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxVulkanTests executables/test)
dsbox_copy_runtime_dlls(DSBoxVulkanTests)

include(CTest)
add_test(NAME DSBoxVulkanTests COMMAND DSBoxVulkanTests)

# Deliberately no SDL_VIDEODRIVER=dummy override here (unlike
# DSBoxSDLTests) -- see the note at the top of main.cpp: this suite
# needs a real display/GPU for most of what it tests, and skips
# gracefully where that's unavailable rather than forcing headlessness.
