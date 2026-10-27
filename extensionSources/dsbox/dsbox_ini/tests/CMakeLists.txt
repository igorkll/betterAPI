add_executable(DSBoxIniTests
    main.cpp
    ini_document_tests.cpp
)

target_link_libraries(DSBoxIniTests PRIVATE DSBox::Ini)

dsbox_set_output_subdir(DSBoxIniTests executables/test)
dsbox_copy_runtime_dlls(DSBoxIniTests)

include(CTest)
add_test(NAME DSBoxIniTests COMMAND DSBoxIniTests)
