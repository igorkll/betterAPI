add_executable(DSBoxModLoaderTests
    main.cpp
    command_line_tests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/command_line.cpp
)

target_include_directories(DSBoxModLoaderTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

dsbox_set_output_subdir(DSBoxModLoaderTests executables/test)
# No DSBox:: shared-library dependency here (command_line.cpp is
# compiled directly into this test, not linked as a library) -- this is
# a harmless no-op, kept for consistency with every other test target.
dsbox_copy_runtime_dlls(DSBoxModLoaderTests)

include(CTest)
add_test(NAME DSBoxModLoaderTests COMMAND DSBoxModLoaderTests)
