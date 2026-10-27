find_package(Vulkan REQUIRED)

add_executable(DSBoxAdbTool
    src/main.cpp
    src/launch_options.cpp
    src/module_config.cpp
    src/adb_format.cpp
    src/gui_mode.cpp
)

target_link_libraries(DSBoxAdbTool PRIVATE
    DSBox::Core
    DSBox::Config
    DSBox::AndroidAdb
    DSBox::SDL
    DSBox::Vulkan
    DSBox::Ui
    DSBox::VulkanGui
    Vulkan::Vulkan
)

if(NOT MSVC)
    target_link_libraries(DSBoxAdbTool PRIVATE Threads::Threads)
endif()

# AndroidAdb already pulls in DSBox::Scheduler as a real binary
# dependency (see dsbox_android_adb/CMakeLists.txt's comment on its
# PRIVATE link) -- its DLL/.so loads automatically with
# DSBoxAndroidAdb's, and dsbox_scheduler::SchedulerModule self-registers
# via DSBOX_STATIC_MODULE the same way dsbox_android_adb::AdbModule does.
# Nothing extra to link here for that.
#
# DSBox::SDL and DSBox::Vulkan are linked for the GUI path (see
# gui_mode.cpp) -- both self-register via DSBOX_STATIC_MODULE, same as
# AndroidAdb/Scheduler above, so registerStaticModules() in main.cpp
# picks them up automatically. DSBox::Ui does NOT self-register (see
# its own CMakeLists.txt comment); gui_mode.cpp registers
# dsbox_ui::UiModule by hand from main.cpp.

set_target_properties(DSBoxAdbTool PROPERTIES
    ENABLE_EXPORTS TRUE
    OUTPUT_NAME "dsbox_adb_tool"
    RUNTIME_OUTPUT_DIRECTORY "${DSBOX_OUTPUT_ROOT}/bin"
)

# Ships config/default/config.ini into the CONSOLIDATED top-level config/
# tree (see docs/BUILD_LAYOUT.md), not next to the executable itself:
# source is the repo-level config/dsbox_adb_tool/default/config.ini (was
# dsbox_adb_tool/config/default/config.ini before the consolidation --
# see the stub left at that old path), and the built copy lands at
# fin-out/config/dsbox_adb_tool/default/config.ini, a sibling of
# fin-out/bin/ (which in turn holds executables/ among other things),
# not nested under it. main.cpp's --pathmode 0 (default) resolves this
# same "<exe dir>/../../config/dsbox_adb_tool" path at runtime -- see
# executableDirectory()/configDir there (the exe itself sits two levels
# under fin-out/, at bin/executables/).
add_custom_command(TARGET DSBoxAdbTool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${DSBOX_OUTPUT_ROOT}/config/dsbox_adb_tool/default"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/dsbox_adb_tool/default/config.ini"
        "${DSBOX_OUTPUT_ROOT}/config/dsbox_adb_tool/default/config.ini"
)
