include(FetchContent)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.x
    GIT_SHALLOW TRUE
)

# Build only what DSBox actually needs from SDL3: no bundled tests,
# examples, or audio (this module doesn't touch audio at all yet).
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3)

add_library(DSBoxSDL SHARED src/window_module.cpp)
add_library(DSBox::SDL ALIAS DSBoxSDL)

target_include_directories(DSBoxSDL PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxSDL PRIVATE DSBOX_SDL_EXPORTING)

# Core is a public dependency: IModule/Context are part of DSBoxSDL's own
# public API. SDL3 is a private implementation detail -- application code
# never sees an SDL type through DSBoxSDL's headers (see WindowHandle in
# window_module.hpp), so consumers of DSBox::SDL don't need to know or
# care that it exists.
target_link_libraries(DSBoxSDL
    PUBLIC DSBox::Core
    PRIVATE SDL3::SDL3
)

if(NOT MSVC)
    target_link_libraries(DSBoxSDL PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
