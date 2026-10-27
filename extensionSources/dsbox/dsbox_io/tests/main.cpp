#include <cstdio>
#include <cstdlib>

void runIOModuleTests();

namespace dsbox_io_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox IO tests\n\n");

    runIOModuleTests();

    std::printf("\n");

    if (dsbox_io_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_io_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
