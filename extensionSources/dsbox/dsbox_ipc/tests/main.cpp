#include <cstdio>
#include <cstdlib>

void runFrameCodecTests();
void runIpcChannelTests();

namespace dsbox_ipc_tests
{
    extern int g_failures;
}

int main()
{
    std::printf("DSBox IPC tests\n\n");

    runFrameCodecTests();
    runIpcChannelTests();

    std::printf("\n");

    if (dsbox_ipc_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_ipc_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
