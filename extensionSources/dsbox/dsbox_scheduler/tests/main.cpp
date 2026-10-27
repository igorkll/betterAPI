#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>

void runTimerQueueTests();
void runExecutorTests();
void runSchedulerTests();
void runCoroutineTests();

int main()
{
    std::printf("DSBox Scheduler tests\n\n");

    runTimerQueueTests();
    runExecutorTests();
    runSchedulerTests();
    runCoroutineTests();

    std::printf("\n");

    if (dsbox_scheduler_tests::g_failures != 0)
    {
        std::printf("Tests failed: %d\n", dsbox_scheduler_tests::g_failures);
        return EXIT_FAILURE;
    }

    std::printf("All tests passed.\n");
    return EXIT_SUCCESS;
}
