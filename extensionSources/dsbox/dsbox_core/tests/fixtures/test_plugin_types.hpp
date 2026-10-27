#pragma once

#include <atomic>

// Shared between the test executable and the fixture plugin .so/.dll so
// the test can observe the plugin's lifecycle calls without needing an
// extra diagnostic export -- both sides just talk to the same
// dsbox::Context service.
namespace dsbox_test_fixtures
{
    struct LifecycleCounters
    {
        std::atomic<int> initCount{ 0 };
        std::atomic<int> startCount{ 0 };
        std::atomic<int> stopCount{ 0 };
        std::atomic<int> shutdownCount{ 0 };
    };
}
