add_executable(DSBoxAndroidAdbTests
    main.cpp
    adb_module_tests.cpp
    adb_device_tests.cpp
)

target_link_libraries(DSBoxAndroidAdbTests PRIVATE DSBox::AndroidAdb)

if(NOT MSVC)
    target_link_libraries(DSBoxAndroidAdbTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxAndroidAdbTests executables/test)
dsbox_copy_runtime_dlls(DSBoxAndroidAdbTests)

include(CTest)
add_test(NAME DSBoxAndroidAdbTests COMMAND DSBoxAndroidAdbTests)
