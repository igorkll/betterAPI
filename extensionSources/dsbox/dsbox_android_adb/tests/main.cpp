#include <cstdio>
#include <cstdlib>

void runAdbModuleTests();
void runAdbDeviceTests();

namespace dsbox_android_adb_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Android ADB tests\n\n");

    runAdbModuleTests();
    runAdbDeviceTests();

    std::printf("\n");

    if (dsbox_android_adb_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_android_adb_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
