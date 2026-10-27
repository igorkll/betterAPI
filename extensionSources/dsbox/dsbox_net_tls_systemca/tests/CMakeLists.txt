add_executable(DSBoxNetTlsSystemCaTests
    main.cpp
    systemca_trust_provider_tests.cpp
    systemca_module_tests.cpp
)

target_link_libraries(DSBoxNetTlsSystemCaTests PRIVATE DSBox::NetTlsSystemCa DSBox::Config)

dsbox_set_output_subdir(DSBoxNetTlsSystemCaTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetTlsSystemCaTests)

include(CTest)
add_test(NAME DSBoxNetTlsSystemCaTests COMMAND DSBoxNetTlsSystemCaTests)
