# replxx gives interactive stdin (enableStdin()) a real line editor: when
# stdin is an actual terminal, it owns the input line itself (raw mode,
# its own echo) so that write()/writeError() can erase-and-redraw around
# whatever the user has typed so far instead of a log line corrupting it
# mid-keystroke. Fetched, not found via find_package(): it's a small,
# header-plus-a-few-.cpp dependency with no system package expected to be
# present, and it's purely an implementation detail of io_module.cpp (not
# mentioned in any public dsbox_io header), so nothing downstream needs to
# know it exists. Piped/non-interactive stdin (tests included) never
# touches it -- see IOModule::Impl::isInteractiveTerminal() in
# io_module.cpp.
include(FetchContent)

set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(REPLXX_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)

# Non-cache, this directory's scope only -- link replxx into DSBoxIO
# statically (no replxx.dll/.so to ship) without forcing every other
# FetchContent'd dependency elsewhere in the build (now or later) to also
# go static via a leaked global cache value.
set(BUILD_SHARED_LIBS OFF)

FetchContent_Declare(
    replxx
    GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
    GIT_TAG release-0.0.4
)
FetchContent_MakeAvailable(replxx)

add_library(DSBoxIO SHARED src/io_module.cpp)
add_library(DSBox::IO ALIAS DSBoxIO)

target_include_directories(DSBoxIO PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxIO PRIVATE DSBOX_IO_EXPORTING)

target_link_libraries(DSBoxIO PUBLIC DSBox::Core DSBox::Process DSBox::Ipc)

# Only io_module.cpp includes replxx.hxx -- keep it out of DSBoxIO's public
# interface so consumers of dsbox_io never need it on their include path.
target_link_libraries(DSBoxIO PRIVATE replxx)

# The shared Scheduler (if any) is looked up in the Context and handed to the
# IPC channels this module creates, so request timeouts share one timer
# thread. Private: no dsbox_io header mentions it. (DSBox::Ipc already links
# it publicly; naming it here keeps this target correct on its own.) Only the
# scheduler LIBRARY -- the module wrapper is not needed.
target_link_libraries(DSBoxIO PRIVATE DSBox::Scheduler)

if(NOT MSVC)
    target_link_libraries(DSBoxIO PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
