add_library(DSBoxCore SHARED
    src/version.cpp
    src/version_requirement.cpp
    src/communication_manager.cpp
    src/module_manager.cpp
    src/module_loader.cpp
    src/static_registry.cpp
    src/window_provider.cpp
    src/text_input_provider.cpp
    src/input.cpp
)
add_library(DSBox::Core ALIAS DSBoxCore)

target_include_directories(DSBoxCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(DSBoxCore
    PRIVATE
        # DSBoxCore is a real shared library (.dll / .so). This define is
        # only set while compiling DSBoxCore's own sources, so DSBOX_API
        # resolves to dllexport here, and (correctly) to dllimport in
        # every other target that merely links against DSBox::Core.
        DSBOX_EXPORTING
        DSBOX_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        DSBOX_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        DSBOX_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        DSBOX_VERSION_STRING="${PROJECT_VERSION}"
)

if(NOT MSVC)
    find_package(Threads REQUIRED)
    target_link_libraries(DSBoxCore PUBLIC Threads::Threads)
endif()

# dlopen/dlsym/dlclose for ModuleLoader on non-Windows platforms.
# CMAKE_DL_LIBS is empty where libdl is not needed, so this is a no-op there.
target_link_libraries(DSBoxCore PRIVATE ${CMAKE_DL_LIBS})

if(DSBOX_BUILD_TESTS)
    add_subdirectory(tests)
endif()