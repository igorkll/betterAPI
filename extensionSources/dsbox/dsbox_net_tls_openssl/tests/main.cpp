#include <cstdio>
#include <cstdlib>

#if !defined(_WIN32)
    #include <csignal>
#endif

void runTlsConnectorTests();
void runHttpsTests();

namespace dsbox_net_tls_openssl_tests
{
    extern int g_failures;
}

int main()
{
#if !defined(_WIN32)
    // The test SERVER writes through OpenSSL, which uses plain write():
    // writing to a client that has already hung up (several tests make
    // exactly that happen) would otherwise kill the whole test process.
    // The code under test never needs this -- its sockets send with
    // MSG_NOSIGNAL.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    std::printf("DSBox Net TLS (OpenSSL) tests\n\n");

    runTlsConnectorTests();
    runHttpsTests();

    std::printf("\n");

    if (dsbox_net_tls_openssl_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_net_tls_openssl_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
