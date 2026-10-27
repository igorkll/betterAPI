# DSBoxModLoader -- the modding entry point. Deliberately kept as its
# OWN subproject (not part of launcher/ or dsbox_core) because it is a
# different kind of artifact entirely: a tiny, dependency-free Win32
# process shim meant to be dropped next to a third-party game's
# executable, not a DSBox host. See Agent.md in this folder.
#
# Windows-only: process injection via CreateProcess/CreateRemoteThread
# is inherently Win32-specific, and the Steam launch-option pattern
# this is built for ("... --main_exec=%command%") is the same on every
# platform Steam ships, but this first version targets Windows titles.

if(NOT WIN32)
    return()
endif()

add_executable(DSBoxModLoader
    src/main.cpp
    src/command_line.cpp
    src/process_guard_win32.cpp
)

target_include_directories(DSBoxModLoader PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(DSBoxModLoader PRIVATE
    # Process enumeration (Toolhelp32) + injection APIs.
    kernel32
    advapi32
)

set_target_properties(DSBoxModLoader PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${DSBOX_OUTPUT_ROOT}"
    # No console window: this is launched by Steam as the game itself,
    # so a console popping up on every play session would just be
    # noise unrelated to the game's own console (if any) -- see the
    # file-level comment in main.cpp. Entry point is wWinMain, not
    # main/wmain, because of this.
    WIN32_EXECUTABLE TRUE
)

# Keep the loader at fin-out/ with multi-config generators too.
if(CMAKE_CONFIGURATION_TYPES)
    foreach(_dsbox_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_dsbox_cfg}" _DSBOX_CFG)
        set_property(TARGET DSBoxModLoader PROPERTY
            "RUNTIME_OUTPUT_DIRECTORY_${_DSBOX_CFG}" "${DSBOX_OUTPUT_ROOT}")
    endforeach()
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
