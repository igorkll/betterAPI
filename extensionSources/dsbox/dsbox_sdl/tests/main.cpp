#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    static void setDummyVideoDriver()
    {
        ::SetEnvironmentVariableA("SDL_VIDEODRIVER", "dummy");
    }
#else
    static void setDummyVideoDriver()
    {
        ::setenv("SDL_VIDEODRIVER", "dummy", 1);
    }
#endif

void runWindowModuleTests();

namespace dsbox_sdl_tests
{
    extern int g_failures;
}

int main()
{
    // Forces SDL's headless "dummy" video driver so these tests never
    // need (or try to open) a real display -- also set via CMake's test
    // ENVIRONMENT property, but set here too so running this binary
    // directly (outside ctest) is just as safe.
    //setDummyVideoDriver();

    std::printf("DSBox SDL tests\n\n");

    runWindowModuleTests();

    std::printf("\n");

    if (dsbox_sdl_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_sdl_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
