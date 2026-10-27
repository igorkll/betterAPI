# dsbox_set_output_subdir(target subdir)
#
# Routes a target's built DLL/.so/.exe into "${CMAKE_BINARY_DIR}/bin/<subdir>"
# instead of the flat "${CMAKE_BINARY_DIR}/bin" every target used to share.
# Import libraries (.lib on Windows) are intentionally left alone -- they
# still go to the shared ARCHIVE_OUTPUT_DIRECTORY ("${CMAKE_BINARY_DIR}/lib"),
# since they're link-time only and were never part of the "DLLs all jumbled
# together" problem this is solving.
#
# Must also pin every per-config output dir (RUNTIME_OUTPUT_DIRECTORY_DEBUG,
# etc.), not just the plain property: on a multi-config generator (Visual
# Studio, Ninja Multi-Config) CMake appends its own "/Debug", "/Release", ...
# subfolder to RUNTIME_OUTPUT_DIRECTORY unless the per-config variant is set
# explicitly too, which would silently fight the layout this function is
# trying to establish.
function(dsbox_set_output_subdir target subdir)
    # DSBOX_OUTPUT_ROOT ("${CMAKE_BINARY_DIR}/fin-out") is set once near
    # the top of the top-level CMakeLists.txt -- every build artifact
    # (bin/, lib/, config/, shaders/) lives under it, not scattered
    # directly in the build directory. Falls back to
    # "${CMAKE_BINARY_DIR}/fin-out" if somehow called before that's set.
    if(NOT DEFINED DSBOX_OUTPUT_ROOT)
        set(DSBOX_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/fin-out")
    endif()

    set(_dsbox_out "${DSBOX_OUTPUT_ROOT}/bin/${subdir}")

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${_dsbox_out}"
        LIBRARY_OUTPUT_DIRECTORY "${_dsbox_out}"
    )

    if(CMAKE_CONFIGURATION_TYPES)
        foreach(_dsbox_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${_dsbox_cfg}" _DSBOX_CFG)
            set_target_properties(${target} PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${_DSBOX_CFG} "${_dsbox_out}"
                LIBRARY_OUTPUT_DIRECTORY_${_DSBOX_CFG} "${_dsbox_out}"
            )
        endforeach()
    endif()
endfunction()
