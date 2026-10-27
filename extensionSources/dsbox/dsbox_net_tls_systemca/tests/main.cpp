#include <cstdio>
#include <cstdlib>

void runSystemCaTrustProviderTests();
void runSystemCaModuleTests();

namespace dsbox_net_tls_systemca_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Net TLS SystemCA tests\n\n");

    runSystemCaTrustProviderTests();
    runSystemCaModuleTests();

    std::printf("\n");

    if (dsbox_net_tls_systemca_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_net_tls_systemca_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
