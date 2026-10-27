include(FetchContent)

# LuaJIT is built from a pinned upstream commit so users don't need to
# install it separately or provide machine-specific include/library paths.
# Keep this as a normal directory-scoped variable, not a cache entry: an
# earlier iteration cached the now-deleted v2.1.0-beta3 tag, which would
# otherwise override the corrected ref on every reconfigure.
set(BETTER_DSBOX_LUAJIT_VERSION "c6ffc141a8762b41703f9287d63d93622a13dd8f")
set(BETTER_DSBOX_LUAJIT_SOURCE_DIR
    "${CMAKE_BINARY_DIR}/_deps/luajit-${BETTER_DSBOX_LUAJIT_VERSION}-src")
FetchContent_Declare(
    LuaJIT
    GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
    GIT_TAG ${BETTER_DSBOX_LUAJIT_VERSION}
    # Upstream removed the old v2.1.0-beta3 tag. Pin a commit from its
    # maintained v2.1 branch instead; full history allows Git to checkout
    # this SHA even when it isn't advertised as a branch or tag.
    GIT_SHALLOW FALSE
    SOURCE_DIR "${BETTER_DSBOX_LUAJIT_SOURCE_DIR}"
)
# Visual Studio configures this project with FETCHCONTENT_FULLY_DISCONNECTED=ON.
# FetchContent reads this option from the cache in its population sub-build,
# so a normal directory variable does not reliably override it. Temporarily
# force both the global and LuaJIT-specific cache options off, then restore
# them so the existing offline policy remains in place for other dependencies.
get_property(_BETTER_DSBOX_GLOBAL_DISCONNECTED_WAS_CACHED
    CACHE FETCHCONTENT_FULLY_DISCONNECTED PROPERTY TYPE SET)
if(_BETTER_DSBOX_GLOBAL_DISCONNECTED_WAS_CACHED)
    get_property(_BETTER_DSBOX_GLOBAL_DISCONNECTED_SAVED
        CACHE FETCHCONTENT_FULLY_DISCONNECTED PROPERTY VALUE)
endif()
get_property(_BETTER_DSBOX_LUAJIT_DISCONNECTED_WAS_CACHED
    CACHE FETCHCONTENT_FULLY_DISCONNECTED_LUAJIT PROPERTY TYPE SET)
if(_BETTER_DSBOX_LUAJIT_DISCONNECTED_WAS_CACHED)
    get_property(_BETTER_DSBOX_LUAJIT_DISCONNECTED_SAVED
        CACHE FETCHCONTENT_FULLY_DISCONNECTED_LUAJIT PROPERTY VALUE)
endif()
set(FETCHCONTENT_FULLY_DISCONNECTED OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_FULLY_DISCONNECTED_LUAJIT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(LuaJIT)
if(_BETTER_DSBOX_GLOBAL_DISCONNECTED_WAS_CACHED)
    set(FETCHCONTENT_FULLY_DISCONNECTED
        "${_BETTER_DSBOX_GLOBAL_DISCONNECTED_SAVED}" CACHE BOOL "" FORCE)
else()
    unset(FETCHCONTENT_FULLY_DISCONNECTED CACHE)
endif()
if(_BETTER_DSBOX_LUAJIT_DISCONNECTED_WAS_CACHED)
    set(FETCHCONTENT_FULLY_DISCONNECTED_LUAJIT
        "${_BETTER_DSBOX_LUAJIT_DISCONNECTED_SAVED}" CACHE BOOL "" FORCE)
else()
    unset(FETCHCONTENT_FULLY_DISCONNECTED_LUAJIT CACHE)
endif()

set(LUAJIT_SOURCE_DIR "${BETTER_DSBOX_LUAJIT_SOURCE_DIR}")
if(NOT EXISTS "${LUAJIT_SOURCE_DIR}/src/lua.h" OR
   NOT EXISTS "${LUAJIT_SOURCE_DIR}/src/lauxlib.h")
    message(FATAL_ERROR
        "LuaJIT sources were not downloaded to ${LUAJIT_SOURCE_DIR}. "
        "Check network access to github.com, then reconfigure CMake.")
endif()
set(LUAJIT_INCLUDE_DIR "${LUAJIT_SOURCE_DIR}/src")

if(WIN32)
    if(MSVC)
        set(LUAJIT_BUILD_SCRIPT "${LUAJIT_SOURCE_DIR}/src/msvcbuild.bat")
        if(NOT EXISTS "${LUAJIT_BUILD_SCRIPT}")
            message(FATAL_ERROR "LuaJIT source checkout is incomplete: missing ${LUAJIT_BUILD_SCRIPT}")
        endif()
    else()
        message(FATAL_ERROR "Automatic LuaJIT builds on Windows currently require MSVC")
    endif()
    # msvcbuild.bat names LuaJIT's import library and runtime after the
    # Lua 5.1 ABI: lua51.lib and lua51.dll.
    set(LUAJIT_LIBRARY "${LUAJIT_SOURCE_DIR}/src/lua51.lib")
    set(LUAJIT_RUNTIME "${LUAJIT_SOURCE_DIR}/src/lua51.dll")
    add_custom_command(
        OUTPUT "${LUAJIT_LIBRARY}" "${LUAJIT_RUNTIME}"
        # These are upstream warnings from LuaJIT's MSVC C build (including
        # intentional narrowing in its VM/parser). Suppress them only for
        # this third-party build; keep DSBox's warning policy unchanged.
        COMMAND ${CMAKE_COMMAND} -E env
            "CL=/wd5287 /wd4244 $ENV{CL}"
            cmd /d /c "${LUAJIT_BUILD_SCRIPT}"
        WORKING_DIRECTORY "${LUAJIT_SOURCE_DIR}/src"
        COMMENT "Building LuaJIT ${BETTER_DSBOX_LUAJIT_VERSION}"
        VERBATIM
    )
    add_custom_target(BetterDSBoxLuaJIT DEPENDS "${LUAJIT_LIBRARY}")
else()
    find_program(LUAJIT_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
    set(LUAJIT_LIBRARY "${LUAJIT_SOURCE_DIR}/src/libluajit.a")
    set(LUAJIT_RUNTIME "")
    add_custom_command(
        OUTPUT "${LUAJIT_LIBRARY}"
        COMMAND "${LUAJIT_MAKE_EXECUTABLE}" BUILDMODE=static
        WORKING_DIRECTORY "${LUAJIT_SOURCE_DIR}"
        COMMENT "Building LuaJIT ${BETTER_DSBOX_LUAJIT_VERSION}"
        VERBATIM
    )
    add_custom_target(BetterDSBoxLuaJIT DEPENDS "${LUAJIT_LIBRARY}")
endif()

add_library(BetterDSBox SHARED src/better_dsbox.cpp)
add_library(BetterDSBox::BetterDSBox ALIAS BetterDSBox)

target_include_directories(BetterDSBox
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${LUAJIT_INCLUDE_DIR}
)
target_compile_definitions(BetterDSBox PRIVATE BETTER_DSBOX_EXPORTING)
target_link_libraries(BetterDSBox PUBLIC DSBox::Core PRIVATE DSBox::SDL SDL3::SDL3 ${LUAJIT_LIBRARY})
add_dependencies(BetterDSBox BetterDSBoxLuaJIT)

# BetterDSBox may be loaded directly by Lua before its framework DLL
# search paths are initialized. Defer its SDL/window imports until after
# bootstrapOwnSearchPath() has run in the exported Lua entry points.
dsbox_delay_load(BetterDSBox DSBoxSDL SDL3-shared)

if(WIN32)
    add_custom_command(TARGET BetterDSBox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LUAJIT_RUNTIME}" "$<TARGET_FILE_DIR:BetterDSBox>"
        COMMENT "Copying LuaJIT runtime next to BetterDSBox"
    )
endif()
