cmake_minimum_required(VERSION 4.0.2)
project(BetterRender)

set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD 23)
include(FetchContent)

# set output path
set(BIN_OUTPUT_PATH ${CMAKE_SOURCE_DIR}/BetterRender)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${BIN_OUTPUT_PATH})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${BIN_OUTPUT_PATH})

# self
include_directories(Source)
file(GLOB_RECURSE SOURCES Source/*.c Source/*.cpp)
add_library(BetterRender SHARED ${SOURCES})
target_compile_definitions(BetterRender PRIVATE BetterRender_EXPORTS)

# luajit
include_directories(${CMAKE_SOURCE_DIR}/../libs)
target_link_libraries(BetterRender PRIVATE ${CMAKE_SOURCE_DIR}/../libs/luajit/lua51.lib)

# winapi
target_link_libraries(BetterRender PRIVATE winhttp user32 dxgi d3d11)
