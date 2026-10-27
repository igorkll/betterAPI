#include "test_framework.hpp"

void runCommandLineTests();

int main()
{
    runCommandLineTests();

    if (dsbox_mod_loader_tests::g_failures > 0)
    {
        std::printf("\n%d failure(s)\n", dsbox_mod_loader_tests::g_failures);
        return 1;
    }

    std::printf("\nAll tests passed\n");
    return 0;
}
