add_library(DSBoxCli SHARED
    src/cli_module.cpp
    src/tokenize.cpp
)
add_library(DSBox::Cli ALIAS DSBoxCli)

target_include_directories(DSBoxCli PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxCli PRIVATE DSBOX_CLI_EXPORTING)

target_link_libraries(DSBoxCli PUBLIC DSBox::Core DSBox::IO)

if(NOT MSVC)
    target_link_libraries(DSBoxCli PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
