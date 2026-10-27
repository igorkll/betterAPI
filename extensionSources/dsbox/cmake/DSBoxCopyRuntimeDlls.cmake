# dsbox_copy_runtime_dlls(target)
#
# For TEST executables/modules only (dsbox_set_output_subdir(... executables/test)).
# The 3 real host executables (DSBoxUiDemo, DSBoxAdbTool, DSBoxLauncher)
# use dsbox_delay_load() + dsbox::bootstrapFrameworkSearchPaths() instead
# (see cmake/DSBoxDelayLoad.cmake) because they need a runtime-configurable
# search path ("[setup] additional_paths="). Tests don't need that --
# they're a dev/CI artifact, never shipped, never reconfigured by a user
# -- so the simpler fix is used here: copy every DLL the target actually,
# transitively depends on straight into its own output directory at
# build time, via CMake's own $<TARGET_RUNTIME_DLLS:...> generator
# expression (CMake 3.21+).
#
# This deliberately does NOT hand-maintain a dependency list the way
# dsbox_delay_load()'s call sites first tried to and got wrong twice
# (see that file's header comment) -- $<TARGET_RUNTIME_DLLS:target>
# asks CMake itself for the full, correct, automatically-updated
# transitive closure of every DLL the target needs at runtime (DSBox::
# libraries, SDL3, OpenSSL, ...), so there's nothing to keep in sync by
# hand when a module's dependencies change.
#
# No-op on a target with no runtime DLL dependencies at all (e.g. a test
# that only links a header-only or static piece), and a no-op on
# non-Windows (ELF RPATH already handles this, same as everywhere else
# in this build). The copy is delegated to a CMake script so an empty
# TARGET_RUNTIME_DLLS list does not leave copy_if_different with only a
# destination argument.
function(dsbox_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DDSBOX_RUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:${target}>,$<SEMICOLON>>"
            "-DDSBOX_RUNTIME_DLLS_DEST=$<TARGET_FILE_DIR:${target}>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/DSBoxCopyRuntimeDllsPostBuild.cmake"
    )
endfunction()
