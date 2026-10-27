add_executable(DSBoxNetTlsTests
    main.cpp
    trust_manager_tests.cpp
    tls_module_tests.cpp
)

target_link_libraries(DSBoxNetTlsTests PRIVATE DSBox::NetTls)

if(NOT MSVC)
    target_link_libraries(DSBoxNetTlsTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetTlsTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetTlsTests)

include(CTest)
add_test(NAME DSBoxNetTlsTests COMMAND DSBoxNetTlsTests)
