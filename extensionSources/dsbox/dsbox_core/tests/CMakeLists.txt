add_subdirectory(fixtures)

add_executable(DSBoxCoreTests
    main.cpp
    communication_manager_tests.cpp
    module_manager_tests.cpp
    module_loader_tests.cpp
    input_tests.cpp
)

target_link_libraries(DSBoxCoreTests PRIVATE DSBox::Core)

# Path to the real .dll/.so fixture plugins, resolved by CMake to
# wherever they actually get built regardless of platform/config, so the
# test executable can ModuleManager::loadDynamic() them for genuine
# cross-library regression coverage instead of only unit-testing the
# host-side code in isolation.
target_compile_definitions(DSBoxCoreTests PRIVATE
    DSBOX_TEST_FIXTURE_PATH="$<TARGET_FILE:DSBoxCoreTestFixture>"
    DSBOX_TEST_FIXTURE_INCOMPATIBLE_PATH="$<TARGET_FILE:DSBoxCoreTestFixtureIncompatible>"
)
add_dependencies(DSBoxCoreTests DSBoxCoreTestFixture DSBoxCoreTestFixtureIncompatible)

# Routed into bin/executables/test/, not the old flat bin/ -- see
# docs/BUILD_LAYOUT.md.
dsbox_set_output_subdir(DSBoxCoreTests executables/test)
dsbox_copy_runtime_dlls(DSBoxCoreTests)

include(CTest)
add_test(NAME DSBoxCoreTests COMMAND DSBoxCoreTests)
