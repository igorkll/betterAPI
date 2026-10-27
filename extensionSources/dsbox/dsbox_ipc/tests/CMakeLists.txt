add_executable(DSBoxIpcTests
    main.cpp
    frame_codec_tests.cpp
    ipc_channel_tests.cpp
)

target_link_libraries(DSBoxIpcTests PRIVATE DSBox::Ipc)

if(NOT MSVC)
    target_link_libraries(DSBoxIpcTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxIpcTests executables/test)
dsbox_copy_runtime_dlls(DSBoxIpcTests)

include(CTest)
add_test(NAME DSBoxIpcTests COMMAND DSBoxIpcTests)
