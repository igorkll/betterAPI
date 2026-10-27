#include <cstdio>
#include <cstdlib>

void runIniDocumentTests();

namespace dsbox_ini_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Ini tests\n\n");

    runIniDocumentTests();

    std::printf("\n");

    if (dsbox_ini_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_ini_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
