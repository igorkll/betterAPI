if(WIN32)
    set(DSBOX_PROCESS_PLATFORM_SRC src/subprocess_win32.cpp)
else()
    set(DSBOX_PROCESS_PLATFORM_SRC src/subprocess_posix.cpp)
endif()

add_library(DSBoxProcess SHARED ${DSBOX_PROCESS_PLATFORM_SRC})
add_library(DSBox::Process ALIAS DSBoxProcess)

target_include_directories(DSBoxProcess PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxProcess PRIVATE DSBOX_PROCESS_EXPORTING)

if(NOT MSVC)
    target_link_libraries(DSBoxProcess PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
