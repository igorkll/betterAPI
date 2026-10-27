# dsbox_delay_load(target dep1 [dep2 ...])
#
# Only needed on an executable's DSBox:: dependencies, and only on MSVC.
# Background:
#
# An executable's implicitly-linked DLLs are normally resolved by the OS
# loader at process creation, before main() runs -- too early for any
# runtime configuration (e.g. a config-file "additional_paths" setting) to
# help, since the framework DLLs now live in per-subsystem subfolders
# instead of next to the exe.
#
# /DELAYLOAD defers resolving a given import DLL until the first actual
# call into it, instead of at process start. That gives main() a chance to
# call AddDllDirectory() for every framework subfolder (see
# dsbox/bootstrap_paths.hpp) BEFORE the first such call happens.
#
# IMPORTANT -- this must list the FULL TRANSITIVE closure of DSBox::
# dependencies the executable implicitly imports, NOT just the targets
# named in the executable's own target_link_libraries() call. CMake
# propagates a PUBLIC dependency onto every consumer's own direct link
# line (and therefore its own PE import table): if the executable links
# DSBox::IO, and DSBoxIO links DSBox::Ipc PUBLIC, and DSBoxIpc links
# DSBox::Scheduler PUBLIC, then the EXECUTABLE itself directly imports
# DSBoxIpc.dll and DSBoxScheduler.dll too -- not just DSBoxIO.dll. An
# earlier version of this comment claimed only an executable's "direct"
# dependencies needed listing here; that was wrong and left
# DSBoxAdbTool/DSBoxLauncher unable to start (DSBoxProcess.dll,
# DSBoxIpc.dll and DSBoxScheduler.dll were implicitly imported but never
# delay-loaded). When adding a new call site, trace every PUBLIC edge
# transitively from what's actually passed to target_link_libraries(),
# not just the first hop -- see docs/BUILD_LAYOUT.md for the current
# computed lists.
#
# A dependency's own PRIVATE-only requirement (e.g. DSBoxSDL.dll
# implicitly importing SDL3.dll, which DSBoxSDL links PRIVATE) does NOT
# need a separate /DELAYLOAD entry here: PRIVATE links don't propagate
# onto the executable's own import table at all, and by the time
# DSBoxSDL.dll itself gets loaded (delayed past process start via this
# mechanism), AddDllDirectory has already widened the process-wide DLL
# search path, so SDL3.dll resolves correctly on its own.
#
# On non-MSVC this is a no-op: ELF shared objects resolve dependencies
# through RPATH entries baked in at link time (see CMAKE_BUILD_RPATH in
# the top-level CMakeLists.txt), which needs no runtime step at all.
function(dsbox_delay_load target)
    if(NOT MSVC)
        return()
    endif()

    if(ARGC LESS 2)
        message(FATAL_ERROR "dsbox_delay_load(${target}): no dependencies given")
    endif()

    set(_dsbox_delay_flags "")
    foreach(_dsbox_dep IN LISTS ARGN)
        list(APPEND _dsbox_delay_flags "/DELAYLOAD:$<TARGET_FILE_NAME:${_dsbox_dep}>")
    endforeach()

    target_link_options(${target} PRIVATE ${_dsbox_delay_flags})

    # delayimp.lib ships with the MSVC toolchain; no find_library needed.
    target_link_libraries(${target} PRIVATE delayimp)
endfunction()
