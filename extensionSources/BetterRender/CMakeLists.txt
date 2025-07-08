# COMPILE VIA MSVC

cmake_minimum_required(VERSION 4.0.2)
project(BetterRender)

set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 23)
include(FetchContent)

# Include directories for source files
include_directories(Source)
include_directories(Library/rlights)

# Gather all source files
file(GLOB_RECURSE SOURCES Source/*.c Source/*.cpp)

# Set output path for the DLL
set(BIN_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/BetterRender)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${BIN_OUTPUT_PATH})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${BIN_OUTPUT_PATH})

# Create a DLL instead of an executable
add_library(BetterRender SHARED ${SOURCES})

# raylib configuration
set(RAYLIB_VERSION 5.5)
FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz
    FIND_PACKAGE_ARGS ${RAYLIB_VERSION}
)
set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(raylib)

# Link raylib to the BetterRender library
target_compile_definitions(raylib PRIVATE EXTERNAL_CONFIG_FLAGS)
target_precompile_headers(raylib PRIVATE Source/raylib_config.h)
target_link_libraries(BetterRender raylib)

# Ensure that you export functions from the DLL
target_compile_definitions(BetterRender PRIVATE BetterRender_EXPORTS)
