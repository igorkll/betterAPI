add_executable(DSBoxNetTlsLocalCaTests
    main.cpp
    pem_parsing_tests.cpp
    localca_trust_provider_tests.cpp
    localca_module_tests.cpp
)

target_link_libraries(DSBoxNetTlsLocalCaTests PRIVATE DSBox::NetTlsLocalCa)

if(NOT MSVC)
    target_link_libraries(DSBoxNetTlsLocalCaTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetTlsLocalCaTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetTlsLocalCaTests)

include(CTest)
add_test(NAME DSBoxNetTlsLocalCaTests COMMAND DSBoxNetTlsLocalCaTests)
