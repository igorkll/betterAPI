# Precompiled headers for DSBox targets.
#
# Only the C++ standard library is precompiled (see dsbox_std_pch.hpp).
#
# Design notes:
#   - PRIVATE: the PCH is an implementation detail of each target's own
#     sources and must never leak to targets that merely link against it.
#   - One PCH per target, no REUSE_FROM: every DSBox library compiles with
#     its own DSBOX_*_EXPORTING definition, and PCH reuse requires
#     identical compile flags.
#   - Applied from the top-level CMakeLists.txt after all add_subdirectory()
#     calls, to the targets of each top-level component directory only. That
#     deliberately skips tests (they live in tests/ subdirectories) and
#     third-party code fetched via FetchContent (SDL3 etc.), which manage
#     their own PCH settings.
#   - Turn it off with -DDSBOX_USE_PCH=OFF (e.g. to check that every source
#     is self-sufficient in its own #includes).

include_guard(GLOBAL)

option(DSBOX_USE_PCH "Use a precompiled header (standard library only) for DSBox targets" ON)

set(DSBOX_STD_PCH_HEADER "${CMAKE_CURRENT_LIST_DIR}/dsbox_std_pch.hpp" CACHE INTERNAL "")

# dsbox_enable_pch(<target>)
# Adds the standard-library PCH to one target. No-op if DSBOX_USE_PCH is OFF
# or the target is not an executable/library (e.g. a custom target).
function(dsbox_enable_pch target)
    if(NOT DSBOX_USE_PCH)
        return()
    endif()

    get_target_property(type ${target} TYPE)
    if(NOT type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY)$")
        return()
    endif()

    # COMPILE_LANGUAGE guard: the header is C++-only and must never be
    # force-included into a C source, should a DSBox target ever get one.
    target_precompile_headers(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${DSBOX_STD_PCH_HEADER}>")
endfunction()

# dsbox_apply_pch()
# Call once at the end of the top-level CMakeLists.txt, after every
# add_subdirectory(): applies dsbox_enable_pch() to each target defined
# directly in one of the top-level component directories.
function(dsbox_apply_pch)
    if(NOT DSBOX_USE_PCH)
        return()
    endif()

    get_property(componentDirs DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY SUBDIRECTORIES)

    foreach(dir IN LISTS componentDirs)
        get_property(dirTargets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)

        foreach(target IN LISTS dirTargets)
            dsbox_enable_pch(${target})
        endforeach()
    endforeach()
endfunction()
