#include <cstdio>
#include <cstdlib>

void runUrlTests();
void runCancellationTests();
void runBodyTests();
void runNetworkManagerTests();

namespace dsbox_net_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Net tests\n\n");

    runUrlTests();
    runCancellationTests();
    runBodyTests();
    runNetworkManagerTests();

    std::printf("\n");

    if (dsbox_net_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_net_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
