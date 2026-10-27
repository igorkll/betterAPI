#include <cstdio>
#include <cstdlib>

void runCliTests();

namespace dsbox_cli_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox CLI tests\n\n");

    runCliTests();

    std::printf("\n");

    if (dsbox_cli_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_cli_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
