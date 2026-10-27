add_library(DSBoxCoreTestFixture MODULE test_plugin.cpp)
target_link_libraries(DSBoxCoreTestFixture PRIVATE DSBox::Core)
# Routed into bin/executables/test/ alongside DSBoxCoreTests itself (see
# docs/BUILD_LAYOUT.md) rather than the old flat bin/ -- DSBoxCoreTests
# finds it via the DSBOX_TEST_FIXTURE_PATH=$<TARGET_FILE:...> generator
# expression in dsbox_core/tests/CMakeLists.txt, which always resolves
# to wherever this actually lands, so no path needs to stay in sync by
# hand.
dsbox_set_output_subdir(DSBoxCoreTestFixture executables/test)
dsbox_copy_runtime_dlls(DSBoxCoreTestFixture)

add_library(DSBoxCoreTestFixtureIncompatible MODULE test_plugin_incompatible.cpp)
target_link_libraries(DSBoxCoreTestFixtureIncompatible PRIVATE DSBox::Core)
dsbox_set_output_subdir(DSBoxCoreTestFixtureIncompatible executables/test)
dsbox_copy_runtime_dlls(DSBoxCoreTestFixtureIncompatible)
