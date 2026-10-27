#include <cstdio>
#include <cstdlib>

void runTcpConnectorTests();
void runConnectorRegistryTests();

namespace dsbox_net_socket_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Net Socket tests\n\n");

    runConnectorRegistryTests();
    runTcpConnectorTests();

    std::printf("\n");

    if (dsbox_net_socket_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_net_socket_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
