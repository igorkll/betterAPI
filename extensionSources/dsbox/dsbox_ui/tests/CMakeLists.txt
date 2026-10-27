add_executable(DSBoxUiTests
    main.cpp
    window_ui_tree_tests.cpp
    stack_layout_tests.cpp
    container_tests.cpp
    text_tests.cpp
    image_tests.cpp
    button_tests.cpp
    text_field_tests.cpp
    text_area_tests.cpp
    list_view_tests.cpp
    scroll_view_tests.cpp
    dropdown_tests.cpp
    progress_bar_tests.cpp
)

target_link_libraries(DSBoxUiTests PRIVATE
    DSBox::Ui
    DSBox::Core
)

if(NOT MSVC)
    target_link_libraries(DSBoxUiTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxUiTests executables/test)
dsbox_copy_runtime_dlls(DSBoxUiTests)

include(CTest)
add_test(NAME DSBoxUiTests COMMAND DSBoxUiTests)
