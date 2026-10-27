# 1. Указываем CMake найти Vulkan SDK, чтобы появилась цель Vulkan::Vulkan
find_package(Vulkan REQUIRED)

# 2. Создаем сам мод (динамическая библиотека-плагин)
add_library(TestWindowMod MODULE test_window_mod.cpp)

# 3. Линкуем все необходимые модули DSBox и Vulkan
target_link_libraries(TestWindowMod PRIVATE 
    DSBox::Core 
    DSBox::SDL
    DSBox::Vulkan
    DSBox::Ui
    DSBox::VulkanGui
    Vulkan::Vulkan
)

# 4. Указываем, что это плагин (для корректного экспорта функций)
target_compile_definitions(TestWindowMod PRIVATE DSBOX_PLUGIN_EXPORTING)

# 5. Настройка путей вывода (чтобы dll оказалась в общей папке bin)
set_target_properties(TestWindowMod PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)