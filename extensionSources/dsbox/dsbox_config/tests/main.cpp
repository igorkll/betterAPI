#include <cstdio>
#include <cstdlib>

void runModuleConfigLoaderTests();
void runPersistentConfigStoreTests();

namespace dsbox_config_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox Config tests\n\n");

    runModuleConfigLoaderTests();
    runPersistentConfigStoreTests();

    std::printf("\n");

    if (dsbox_config_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_config_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
