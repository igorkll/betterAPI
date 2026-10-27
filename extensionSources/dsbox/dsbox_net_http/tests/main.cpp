#include <cstdio>
#include <cstdlib>

void runHttp1Tests();
void runHttpHandlerTests();

namespace dsbox_net_http_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Net HTTP tests\n\n");

    runHttp1Tests();
    runHttpHandlerTests();

    std::printf("\n");

    if (dsbox_net_http_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_net_http_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
