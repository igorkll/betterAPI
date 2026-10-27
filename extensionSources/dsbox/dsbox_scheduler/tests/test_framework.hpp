#pragma once

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>

namespace dsbox_scheduler_tests
{
    inline int g_failures = 0;

    inline void check(bool condition, const char* expression, const char* test)
    {
        if (condition)
            return;

        std::printf("[FAIL] %s: %s\n", test, expression);
        ++g_failures;
    }

    inline void runTest(const char* name, void (*test)())
    {
        const int oldFailures = g_failures;

        std::printf("[TEST] %s\n", name);

        try
        {
            test();
        }
        catch (const std::exception& exception)
        {
            std::printf("[FAIL] %s: unexpected exception: %s\n", name, exception.what());
            ++g_failures;
        }
        catch (...)
        {
            std::printf("[FAIL] %s: unexpected unknown exception\n", name);
            ++g_failures;
        }

        if (g_failures == oldFailures)
            std::printf("[PASS] %s\n", name);
    }

    // Polls `condition` until it holds or `timeout` passes. For the few
    // tests that genuinely have to wait on another thread; everything
    // else in this suite uses ManualClock and never sleeps.
    template<typename Predicate>
    bool waitFor(Predicate condition, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return true;
    }
}

#define DSBOX_SCHEDULER_CHECK(expr) \
    ::dsbox_scheduler_tests::check(static_cast<bool>(expr), #expr, __func__)

#define DSBOX_SCHEDULER_CHECK_EQ(a, b) \
    ::dsbox_scheduler_tests::check(((a) == (b)), #a " == " #b, __func__)
