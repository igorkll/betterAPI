add_library(DSBoxIni SHARED
    src/ini_document.cpp
)
add_library(DSBox::Ini ALIAS DSBoxIni)

target_include_directories(DSBoxIni PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxIni PRIVATE DSBOX_INI_EXPORTING)

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
