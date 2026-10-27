add_executable(DSBoxNetHttpTests
    main.cpp
    http1_tests.cpp
    http_handler_tests.cpp
)

target_link_libraries(DSBoxNetHttpTests PRIVATE DSBox::NetHttp)

# The loopback test server lives with the socket tests and is shared by
# these; it talks to the OS sockets directly, independent of the code
# under test.
target_include_directories(DSBoxNetHttpTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../dsbox_net_socket/tests"
)

if(WIN32)
    target_link_libraries(DSBoxNetHttpTests PRIVATE ws2_32)
elseif(NOT MSVC)
    target_link_libraries(DSBoxNetHttpTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetHttpTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetHttpTests)

include(CTest)
add_test(NAME DSBoxNetHttpTests COMMAND DSBoxNetHttpTests)
