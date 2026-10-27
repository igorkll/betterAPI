#include <cstdio>
#include <cstdlib>

void runSubprocessTests();

namespace dsbox_process_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Process tests\n\n");

    runSubprocessTests();

    std::printf("\n");

    if (dsbox_process_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_process_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
