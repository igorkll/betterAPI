add_executable(DSBoxNetTests
    main.cpp
    url_tests.cpp
    cancellation_tests.cpp
    body_tests.cpp
    network_manager_tests.cpp
)

target_link_libraries(DSBoxNetTests PRIVATE DSBox::Net)

if(NOT MSVC)
    target_link_libraries(DSBoxNetTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxNetTests executables/test)
dsbox_copy_runtime_dlls(DSBoxNetTests)

include(CTest)
add_test(NAME DSBoxNetTests COMMAND DSBoxNetTests)
