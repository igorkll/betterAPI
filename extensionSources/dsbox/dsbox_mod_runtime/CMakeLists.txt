# DSBoxModRuntime -- the DLL DSBoxModLoader injects into the target
# game process. See Agent.md in this folder and in dsbox_mod_loader/.
#
# Windows-only for the same reason dsbox_mod_loader is: DllMain and the
# injection it is loaded by are both Win32-specific in this first pass.

if(NOT WIN32)
    return()
endif()

add_library(DSBoxModRuntime SHARED src/runtime_entry.cpp)
add_library(DSBox::ModRuntime ALIAS DSBoxModRuntime)

target_include_directories(DSBoxModRuntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxModRuntime PRIVATE DSBOX_MOD_RUNTIME_EXPORTING)

target_link_libraries(DSBoxModRuntime PRIVATE
    DSBox::Core
    DSBox::Config
)

# The runtime belongs to the shared framework tree. Its DSBox dependencies
# are emitted to their normal framework subsystem directories; the loader
# adds framework/core to the target process's DLL search path before load.
set_target_properties(DSBoxModRuntime PROPERTIES
    OUTPUT_NAME "DSBoxModRuntime"
    RUNTIME_OUTPUT_DIRECTORY "${DSBOX_OUTPUT_ROOT}/bin/framework/mod"
)

# Keep the package path stable with multi-config generators too.
if(CMAKE_CONFIGURATION_TYPES)
    foreach(_dsbox_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${_dsbox_cfg}" _DSBOX_CFG)
        set_target_properties(DSBoxModRuntime PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_DSBOX_CFG} "${DSBOX_OUTPUT_ROOT}/bin/framework/mod"
        )
    endforeach()
endif()

# Runtime dependencies stay in their shared output locations (for example,
# DSBoxCore/Config/Ini under bin/framework/core/). Do not duplicate them in
# the mod runtime directory. DSBoxModLoader additionally configures the
# target process's DLL search path before loading this DLL, but that is
# no longer load-bearing: this DLL widens its OWN search path itself, as
# the first thing DSBoxRuntimeInitialize() does (see runtime_entry.cpp),
# precisely so it also works when loaded by a host other than
# DSBoxModLoader (e.g. a game's embedded LuaJIT calling ffi.load() on it
# directly). That self-bootstrap requires DSBoxCore/DSBoxConfig to be
# delay-loaded for this target -- see the dsbox_delay_load(DSBoxModRuntime
# ...) call in the top-level CMakeLists.txt.
