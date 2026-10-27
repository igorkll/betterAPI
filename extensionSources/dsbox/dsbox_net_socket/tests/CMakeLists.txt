add_executable(DSBoxNetSocketTests
    main.cpp
    connector_registry_tests.cpp
    tcp_connector_tests.cpp
)

target_link_libraries(DSBoxNetSocketTests PRIVATE DSBox::NetSocket)

# The loopback test server (loopback_server.hpp) talks to the OS sockets
# directly, independent of the library under test.
if(WIN32)
    target_link_libraries(DSBoxNetSocketTests PRIVATE ws2_32)
elseif(NOT MSVC)
    target_link_libraries(DSBoxNetSocketTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetSocketTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetSocketTests)

include(CTest)
add_test(NAME DSBoxNetSocketTests COMMAND DSBoxNetSocketTests)
