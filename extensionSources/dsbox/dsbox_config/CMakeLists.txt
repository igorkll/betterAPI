add_library(DSBoxConfig SHARED
    src/module_config_loader.cpp
    src/persistent_config_store.cpp
)
add_library(DSBox::Config ALIAS DSBoxConfig)

target_include_directories(DSBoxConfig PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxConfig PRIVATE DSBOX_CONFIG_EXPORTING)

# Core is a public dependency: the public API (loadFromIni) takes a
# dsbox::ModuleManager&, so callers need Core's headers regardless.
# Ini is a private implementation detail -- it's used only inside the
# .cpp to parse the config text and never appears in a public header, so
# consumers of DSBoxConfig don't need to know it exists.
target_link_libraries(DSBoxConfig
    PUBLIC DSBox::Core
    PRIVATE DSBox::Ini
)

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
