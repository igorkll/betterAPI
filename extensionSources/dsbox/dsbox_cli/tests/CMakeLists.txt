add_executable(DSBoxCliTests
    main.cpp
    cli_tests.cpp
)

target_link_libraries(DSBoxCliTests PRIVATE DSBox::Cli)

if(NOT MSVC)
    target_link_libraries(DSBoxCliTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxCliTests executables/test)
dsbox_copy_runtime_dlls(DSBoxCliTests)

include(CTest)
add_test(NAME DSBoxCliTests COMMAND DSBoxCliTests)
