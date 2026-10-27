# 3.21 (not the original 3.16) because test binaries' DLL copying uses
# $<TARGET_RUNTIME_DLLS:...> (added in 3.21) -- see
# cmake/DSBoxCopyRuntimeDlls.cmake.
cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
endif()

project(DSBox VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    # C++ sources use standard exceptions throughout DSBox. State the model
    # explicitly so it does not depend on the generator's default flags.
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHsc>")
else()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    find_package(Threads REQUIRED)
endif()

if(WIN32)
    set(CMAKE_ENABLE_EXPORTS ON)
endif()

# Every build artifact (bin/, lib/, config/, shaders/, ...) lives under
# one top-level output root, ${CMAKE_BINARY_DIR}/fin-out/, instead of
# scattered directly in the build directory -- see docs/BUILD_LAYOUT.md.
# Referenced from cmake/DSBoxOutputDirs.cmake (dsbox_set_output_subdir(),
# which prefixes every subdir with "${DSBOX_OUTPUT_ROOT}/bin/"),
# dsbox_adb_tool/CMakeLists.txt and dsbox_vulkan_gui/CMakeLists.txt's own
# POST_BUILD copy steps and dsbox_mod_loader/CMakeLists.txt.
set(DSBOX_OUTPUT_ROOT "${CMAKE_BINARY_DIR}/fin-out")

# DSBox libraries (Core, Ini, Config, Net, ...) are real shared libraries
# (.dll / .so), distributed across fin-out/bin/framework/<subsystem>/,
# fin-out/bin/plugins/ and fin-out/bin/executables/ (see
# cmake/DSBoxOutputDirs.cmake and the dsbox_set_output_subdir() calls
# below) rather than one flat folder. Only the ARCHIVE output (Windows
# .lib import libraries, link-time only) stays in one shared place -- it
# was never part of the "DLLs all jumbled together" problem this split
# is solving.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${DSBOX_OUTPUT_ROOT}/lib")

# Any target that doesn't get an explicit dsbox_set_output_subdir() call
# below (e.g. a future module someone forgets to route) still lands
# somewhere sane instead of silently reverting to the top of the build
# directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DSBOX_OUTPUT_ROOT}/bin/unsorted")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${DSBOX_OUTPUT_ROOT}/bin/unsorted")

if(UNIX)
    # On Linux/macOS, .so files are LIBRARY artifacts (not RUNTIME).
    # With every subsystem now in its own subfolder, a single "$ORIGIN"
    # RPATH entry (which only covers a binary's own directory) is no
    # longer enough -- each binary also needs every OTHER subsystem
    # subfolder its dependencies might live in. Rather than hand-tune a
    # minimal per-target RPATH, every framework subfolder is added to
    # every target's RPATH: harmless (a missing directory is just
    # skipped by the dynamic loader) and far simpler than keeping a
    # per-target dependency-to-RPATH mapping in sync by hand.
    set(CMAKE_BUILD_RPATH
        "$ORIGIN"
        "$ORIGIN/../core"
        "$ORIGIN/../scheduler"
        "$ORIGIN/../ipc"
        "$ORIGIN/../io"
        "$ORIGIN/../android"
        "$ORIGIN/../cli"
        "$ORIGIN/../net"
        "$ORIGIN/../net/tls"
        "$ORIGIN/../ui"
        "$ORIGIN/../render"
        "$ORIGIN/../../framework/core"
        "$ORIGIN/../../framework/scheduler"
        "$ORIGIN/../../framework/ipc"
        "$ORIGIN/../../framework/io"
        "$ORIGIN/../../framework/android"
        "$ORIGIN/../../framework/cli"
        "$ORIGIN/../../framework/net"
        "$ORIGIN/../../framework/net/tls"
        "$ORIGIN/../../framework/ui"
        "$ORIGIN/../../framework/render"
    )
endif()

option(DSBOX_BUILD_TESTS "Build DSBox unit tests" ON)

# The TLS implementation (dsbox_net_tls_openssl) needs OpenSSL 1.1.1 or
# newer, headers and libraries both.
#   AUTO (default) -- build it if OpenSSL is found, otherwise skip it with
#                     a notice; https:// then simply has no transport.
#   ON             -- OpenSSL is required; configuring fails without it.
#   OFF            -- never build it.
# Where OpenSSL lives is found the usual way (OPENSSL_ROOT_DIR, a vcpkg or
# Conan toolchain, the system's development package).
set(DSBOX_WITH_OPENSSL "AUTO" CACHE STRING "Build dsbox_net_tls_openssl: AUTO, ON or OFF")
set_property(CACHE DSBOX_WITH_OPENSSL PROPERTY STRINGS AUTO ON OFF)

set(DSBOX_HAVE_OPENSSL FALSE)

if(DSBOX_WITH_OPENSSL STREQUAL "ON")
    find_package(OpenSSL 1.1.1 REQUIRED)
    set(DSBOX_HAVE_OPENSSL TRUE)
elseif(NOT DSBOX_WITH_OPENSSL STREQUAL "OFF")
    find_package(OpenSSL 1.1.1)

    if(OPENSSL_FOUND)
        set(DSBOX_HAVE_OPENSSL TRUE)
    else()
        message(STATUS "OpenSSL not found: dsbox_net_tls_openssl (https://) will not be built. "
                       "Set OPENSSL_ROOT_DIR, or DSBOX_WITH_OPENSSL=OFF to silence this.")
    endif()
endif()

if(DSBOX_BUILD_TESTS)
    # Must be called at the top level (not just inside each component's
    # tests/CMakeLists.txt) so CMake generates the root CTestTestfile.cmake
    # with subdirs() entries chaining down to every component -- without
    # this, `ctest` run from the top of the build directory finds nothing
    # even though each component's own tests are correctly registered.
    include(CTest)
endif()

# Precompiled standard-library header (option DSBOX_USE_PCH, default ON).
# Defines dsbox_apply_pch(), called once after all add_subdirectory()
# calls below.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(DSBoxPch)
include(DSBoxOutputDirs)
include(DSBoxDelayLoad)
include(DSBoxCopyRuntimeDlls)

add_subdirectory(dsbox_core)
add_subdirectory(dsbox_ini)
add_subdirectory(dsbox_config)
add_subdirectory(dsbox_scheduler)
add_subdirectory(dsbox_net)
add_subdirectory(dsbox_net_file)
add_subdirectory(dsbox_net_tls)
add_subdirectory(dsbox_net_tls_localca)
add_subdirectory(dsbox_net_tls_systemca)
add_subdirectory(dsbox_net_socket)
add_subdirectory(dsbox_net_http)
if(DSBOX_HAVE_OPENSSL)
    add_subdirectory(dsbox_net_tls_openssl)
endif()
add_subdirectory(dsbox_process)
add_subdirectory(dsbox_ipc)
add_subdirectory(dsbox_io)
add_subdirectory(dsbox_android_adb)
add_subdirectory(dsbox_cli)
add_subdirectory(dsbox_sdl)
add_subdirectory(better_dsbox)
add_subdirectory(dsbox_vulkan)
add_subdirectory(dsbox_ui)
add_subdirectory(dsbox_vulkan_gui)
add_subdirectory(dsbox_ui_demo)
add_subdirectory(launcher)
add_subdirectory(dsbox_adb_tool)

# Modding entry point: a standalone loader EXE and an injected runtime DLL.
add_subdirectory(dsbox_mod_loader)
add_subdirectory(dsbox_mod_runtime)
add_subdirectory(test_window_mod)

# -----------------------------------------------------------------------
# Output layout: route every target into its
# fin-out/bin/framework/<subsystem>/, fin-out/bin/plugins/ or
# fin-out/bin/executables/ subfolder (see cmake/DSBoxOutputDirs.cmake,
# which prefixes every subdir passed below with "${DSBOX_OUTPUT_ROOT}/bin/").
# Centralized here rather than in each component's own CMakeLists.txt so
# the whole mapping is visible and editable in one place.
#
# DSBoxModLoader is routed to fin-out/ root; DSBoxModRuntime uses the shared
# fin-out/bin/framework/mod/ directory, alongside framework/core/ dependencies.
# -----------------------------------------------------------------------

# framework/core -- small, dependency-light utility libraries.
dsbox_set_output_subdir(DSBoxCore     framework/core)
dsbox_set_output_subdir(DSBoxIni      framework/core)
dsbox_set_output_subdir(DSBoxConfig   framework/core)
dsbox_set_output_subdir(DSBoxProcess  framework/core)

# framework/scheduler
dsbox_set_output_subdir(DSBoxScheduler       framework/scheduler)
dsbox_set_output_subdir(DSBoxSchedulerModule framework/scheduler)

# framework/ipc, framework/io, framework/android, framework/cli
dsbox_set_output_subdir(DSBoxIpc        framework/ipc)
dsbox_set_output_subdir(DSBoxIO         framework/io)
dsbox_set_output_subdir(DSBoxAndroidAdb framework/android)
dsbox_set_output_subdir(DSBoxCli        framework/cli)

# framework/net (+ framework/net/tls for the TLS family)
dsbox_set_output_subdir(DSBoxNet        framework/net)
dsbox_set_output_subdir(DSBoxNetFile    framework/net)
dsbox_set_output_subdir(DSBoxNetHttp    framework/net)
dsbox_set_output_subdir(DSBoxNetSocket  framework/net)
dsbox_set_output_subdir(DSBoxNetTls         framework/net/tls)
dsbox_set_output_subdir(DSBoxNetTlsLocalCa  framework/net/tls)
dsbox_set_output_subdir(DSBoxNetTlsSystemCa framework/net/tls)
if(TARGET DSBoxNetTlsOpenSsl)
    dsbox_set_output_subdir(DSBoxNetTlsOpenSsl framework/net/tls)
endif()

# framework/ui -- SDL's own built SDL3.dll/.so goes in a module-specific
# subfolder nested under the module that bundles it, same pattern any
# future module with its own private runtime dependency should follow.
dsbox_set_output_subdir(DSBoxSDL framework/ui)
if(TARGET BetterDSBox)
    dsbox_set_output_subdir(BetterDSBox plugins)
endif()
dsbox_set_output_subdir(DSBoxUi  framework/ui)
if(TARGET SDL3-shared)
    dsbox_set_output_subdir(SDL3-shared framework/ui/DSBoxSDL)
endif()

# framework/render
dsbox_set_output_subdir(DSBoxVulkan    framework/render)
dsbox_set_output_subdir(DSBoxVulkanGui framework/render)

# executables
dsbox_set_output_subdir(DSBoxUiDemo   executables)
dsbox_set_output_subdir(DSBoxAdbTool  executables)
dsbox_set_output_subdir(DSBoxLauncher executables)

# plugins -- dynamically loaded via ModuleLoader, never implicitly linked.
dsbox_set_output_subdir(TestWindowMod plugins)

# -----------------------------------------------------------------------
# Delay-loading: any binary that implicitly links DSBox:: libraries needs
# this -- and each call below must list the FULL
# transitive PUBLIC closure of DSBox:: dependencies, not just what's
# named in that binary's own target_link_libraries() call (see
# cmake/DSBoxDelayLoad.cmake's header comment for why, and the trap that
# caught DSBoxAdbTool/DSBoxLauncher the first time around). DSBoxModLoader
# has no DSBox library dependency.
# -----------------------------------------------------------------------
# DSBoxUiDemo's FULL transitive PUBLIC closure: SDL, Vulkan and Ui each
# only PUBLIC-pull Core (already listed); VulkanGui PUBLIC-pulls Core,
# Ui, Vulkan (already listed). Nothing else to add.
dsbox_delay_load(DSBoxUiDemo
    DSBoxCore DSBoxSDL DSBoxVulkan DSBoxUi DSBoxVulkanGui
)

# DSBoxAdbTool's FULL transitive PUBLIC closure (see
# cmake/DSBoxDelayLoad.cmake's header comment for why this must be the
# full closure, not just what's named in dsbox_adb_tool/CMakeLists.txt's
# own target_link_libraries() call): Core, Config, AndroidAdb, SDL,
# Vulkan, Ui, VulkanGui are direct. AndroidAdb also links Process for its
# own implementation; the executable does not import Process directly, so
# that DLL is resolved when AndroidAdb is loaded after the search paths are
# configured.
dsbox_delay_load(DSBoxAdbTool
    DSBoxCore DSBoxConfig DSBoxAndroidAdb
    DSBoxSDL DSBoxVulkan DSBoxUi DSBoxVulkanGui
)

# DSBoxLauncher's FULL transitive PUBLIC closure: Core, IO, Cli, Config
# are direct; IO pulls in Process, Ipc, Scheduler the same way as above.
# The net stack (when DSBOX_LAUNCHER_WITH_NET) adds Net, NetFile,
# NetTls(+LocalCa/+SystemCa), NetSocket, NetHttp(+OpenSsl) directly --
# Net itself also PUBLIC-pulls Scheduler (already covered via IO).
set(_dsbox_launcher_deps
    DSBoxCore DSBoxIO DSBoxCli DSBoxConfig
    DSBoxProcess DSBoxIpc DSBoxScheduler
)
if(DSBOX_LAUNCHER_WITH_NET)
    list(APPEND _dsbox_launcher_deps
        DSBoxNet DSBoxNetFile DSBoxNetTls DSBoxNetTlsLocalCa
        DSBoxNetTlsSystemCa DSBoxNetSocket DSBoxNetHttp
    )
    if(TARGET DSBoxNetTlsOpenSsl)
        list(APPEND _dsbox_launcher_deps DSBoxNetTlsOpenSsl)
    endif()
endif()
dsbox_delay_load(DSBoxLauncher ${_dsbox_launcher_deps})
unset(_dsbox_launcher_deps)

# DSBoxModRuntime's own direct deps: DSBoxCore, DSBoxConfig (Config's own
# PRIVATE dependency on Ini resolves fine once Config.dll itself loads,
# same reasoning as SDL3.dll under DSBoxSDL -- see cmake/DSBoxDelayLoad.cmake).
# Needed because, unlike the three executables above, DSBoxModRuntime is
# not always loaded by DSBoxModLoader (which used to configure the
# search path for it via a remote SetDllDirectoryW call before
# injecting): a host that loads this DLL directly -- e.g. a game's
# embedded LuaJIT calling ffi.load() on it -- never does that, so
# DSBoxModRuntime must be able to widen its own search path from inside
# DSBoxRuntimeInitialize() (see runtime_entry.cpp), which requires its
# own imports to still be unresolved at that point.
if(TARGET DSBoxModRuntime)
    dsbox_delay_load(DSBoxModRuntime DSBoxCore DSBoxConfig)
endif()

# BetterDSBox's own direct dep: DSBoxCore (PUBLIC target_link_libraries
# in better_dsbox/CMakeLists.txt). Same reasoning as DSBoxModRuntime
# above: BetterDSBox.dll is no longer only loaded by ModuleManager
# (after DSBoxModRuntime has already widened the process's search
# path) -- it is now ALSO loadable directly as a standard Lua C module
# via require("BetterDSBox")/package.loadlib(), which calls its
# luaopen_BetterDSBox() export and can run before, or with no,
# DSBoxModRuntime involved at all (see better_dsbox.cpp's
# bootstrapOwnSearchPath()).
if(TARGET BetterDSBox)
    dsbox_delay_load(BetterDSBox DSBoxCore)
endif()

# After every component exists: attach the PCH to each component's own
# targets (not tests, not FetchContent'd third-party code).
dsbox_apply_pch()
