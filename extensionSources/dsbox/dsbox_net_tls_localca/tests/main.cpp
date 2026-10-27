#include <cstdio>
#include <cstdlib>

void runPemParsingTests();
void runLocalCaTrustProviderTests();
void runLocalCaModuleTests();

namespace dsbox_net_tls_localca_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Net TLS LocalCA tests\n\n");

    runPemParsingTests();
    runLocalCaTrustProviderTests();
    runLocalCaModuleTests();

    std::printf("\n");

    if (dsbox_net_tls_localca_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_net_tls_localca_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
