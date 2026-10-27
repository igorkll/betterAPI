add_executable(DSBoxProcessTests
    main.cpp
    subprocess_tests.cpp
)

target_link_libraries(DSBoxProcessTests PRIVATE DSBox::Process)

if(NOT MSVC)
    target_link_libraries(DSBoxProcessTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxProcessTests executables/test)
dsbox_copy_runtime_dlls(DSBoxProcessTests)

include(CTest)
add_test(NAME DSBoxProcessTests COMMAND DSBoxProcessTests)
