#pragma once

#include <cstdio>
#include <exception>

namespace dsbox_ini_tests
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
}

#define DSBOX_INI_CHECK(expr) \
    ::dsbox_ini_tests::check(static_cast<bool>(expr), #expr, __func__)

#define DSBOX_INI_CHECK_EQ(a, b) \
    ::dsbox_ini_tests::check(((a) == (b)), #a " == " #b, __func__)
