add_executable(DSBoxSDLTests
    main.cpp
    window_module_tests.cpp
)

target_link_libraries(DSBoxSDLTests PRIVATE
    DSBox::SDL
    SDL3::SDL3
)

if(NOT MSVC)
    target_link_libraries(DSBoxSDLTests PRIVATE Threads::Threads)
endif()

dsbox_set_output_subdir(DSBoxSDLTests executables/test)
dsbox_copy_runtime_dlls(DSBoxSDLTests)

include(CTest)
add_test(NAME DSBoxSDLTests COMMAND DSBoxSDLTests)

# Forces the headless "dummy" video driver so this test never needs (or
# tries to open) a real display -- the test binary's own main() also
# sets this, this is belt-and-suspenders for anyone invoking it via
# ctest directly.
set_tests_properties(DSBoxSDLTests PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
