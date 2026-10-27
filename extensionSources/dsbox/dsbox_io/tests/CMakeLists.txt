add_executable(DSBoxIOTests
    main.cpp
    io_module_tests.cpp
)

target_link_libraries(DSBoxIOTests PRIVATE DSBox::IO DSBox::Scheduler)

if(NOT MSVC)
    target_link_libraries(DSBoxIOTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxIOTests executables/test)
dsbox_copy_runtime_dlls(DSBoxIOTests)

include(CTest)
add_test(NAME DSBoxIOTests COMMAND DSBoxIOTests)
