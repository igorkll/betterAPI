add_executable(DSBoxNetTlsOpenSslTests
    main.cpp
    tls_connector_tests.cpp
    https_tests.cpp
)

# The https tests drive the whole client stack, so NetHttp is a test-only
# dependency here. OpenSSL is linked directly because the tests play the
# server (and generate their own certificates) with it.
target_link_libraries(DSBoxNetTlsOpenSslTests PRIVATE
    DSBox::NetTlsOpenSsl
    DSBox::NetHttp
    OpenSSL::SSL
    OpenSSL::Crypto
)

# The loopback test server lives with the socket tests.
target_include_directories(DSBoxNetTlsOpenSslTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../dsbox_net_socket/tests"
)

if(WIN32)
    target_link_libraries(DSBoxNetTlsOpenSslTests PRIVATE ws2_32)
elseif(NOT MSVC)
    target_link_libraries(DSBoxNetTlsOpenSslTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetTlsOpenSslTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetTlsOpenSslTests)

include(CTest)
add_test(NAME DSBoxNetTlsOpenSslTests COMMAND DSBoxNetTlsOpenSslTests)
