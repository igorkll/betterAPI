#include <dsbox/version.hpp>

#include <cstdio>
#include <cstdlib>

void runCommunicationManagerTests();
void runModuleManagerTests();
void runModuleLoaderTests();
void runInputTests();

namespace dsbox_tests
{
    extern int g_failures;
}

int main()
{
    std::printf(
        "DSBox Core tests - version %u.%u.%u (%s)\n\n",
        dsbox::versionMajor(),
        dsbox::versionMinor(),
        dsbox::versionPatch(),
        dsbox::versionString()
    );

    runCommunicationManagerTests();
    runModuleManagerTests();
    runModuleLoaderTests();
    runInputTests();

    std::printf("\n");

    if (dsbox_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
