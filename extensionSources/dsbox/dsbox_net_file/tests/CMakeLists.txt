add_executable(DSBoxNetFileTests
    main.cpp
    path_style_tests.cpp
    file_protocol_handler_tests.cpp
    file_module_tests.cpp
)

target_link_libraries(DSBoxNetFileTests PRIVATE DSBox::NetFile DSBox::Config)

if(NOT MSVC)
    target_link_libraries(DSBoxNetFileTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetFileTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetFileTests)

include(CTest)
add_test(NAME DSBoxNetFileTests COMMAND DSBoxNetFileTests)
