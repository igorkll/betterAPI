add_library(DSBoxUi SHARED
    src/window_ui_tree.cpp
    src/ui_module.cpp
    src/container.cpp
    src/stack_layout.cpp
    src/text.cpp
    src/image.cpp
    src/button.cpp
    src/text_field.cpp
    src/text_area.cpp
    src/list_view.cpp
    src/scroll_view.cpp
    src/dropdown.cpp
    src/progress_bar.cpp
    src/panel.cpp
)
add_library(DSBox::Ui ALIAS DSBoxUi)

target_include_directories(DSBoxUi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_definitions(DSBoxUi PRIVATE DSBOX_UI_EXPORTING)

# Core is a public dependency: IModule/Context/CommunicationManager/
# Input/ITextInputProvider are part of DSBoxUi's own public API, same
# as every other module.
#
# Note there is deliberately NO DSBox::SDL and NO DSBox::Vulkan/
# DSBox::VulkanGui here, public or private: WindowUiTree/UiModule speak
# only dsbox::WindowHandle, dsbox::ITextInputProvider and this module's
# own IUiRenderBackend contract -- never a concrete windowing or
# rendering module. dsbox_vulkan_gui is the (so far only) module that
# depends on DSBox::Ui, not the other way around.
target_link_libraries(DSBoxUi
    PUBLIC
        DSBox::Core
)

if(NOT MSVC)
    target_link_libraries(DSBoxUi PRIVATE Threads::Threads)
endif()

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
