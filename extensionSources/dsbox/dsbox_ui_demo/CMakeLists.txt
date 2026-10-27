find_package(Vulkan REQUIRED)

add_executable(DSBoxUiDemo src/main.cpp)

target_link_libraries(DSBoxUiDemo PRIVATE
    DSBox::Core
    DSBox::SDL
    DSBox::Vulkan
    DSBox::Ui
    DSBox::VulkanGui
    Vulkan::Vulkan
)

if(NOT MSVC)
    target_link_libraries(DSBoxUiDemo PRIVATE Threads::Threads)
endif()

# No explicit RUNTIME_OUTPUT_DIRECTORY override needed here -- the root
# CMakeLists.txt already routes every DLL/EXE (and dsbox_vulkan_gui's
# copied .spv shaders) into the same "${CMAKE_BINARY_DIR}/bin", which is
# exactly what main.cpp's executableDirectory() helper relies on to
# find "shaders/dsbox_vulkan_gui" next to itself.
