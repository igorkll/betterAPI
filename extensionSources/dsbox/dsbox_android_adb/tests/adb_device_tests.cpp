#include "test_framework.hpp"

#include <dsbox_android_adb/adb_device.hpp>
#include <dsbox_android_adb/adb_module.hpp>
#include <dsbox_android_adb/command_executor.hpp>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dsbox_android_adb_tests;
using namespace dsbox_android_adb;

namespace
{
    /*
     * AdbCommandExecutor -- behavior that doesn't require a real `adb`
     * on PATH
     */

    void testDefaultAdbOptionsIncludesCommandThreads()
    {
        AdbOptions options;
        DSBOX_ANDROID_ADB_CHECK_EQ(options.commandThreads, static_cast<std::size_t>(0));
    }

    void testRunThrowsForMissingAdbExecutable()
    {
        AdbCommandExecutor executor("dsbox_definitely_not_a_real_adb_xyz");

        bool threw = false;
        try
        {
            executor.run("some-serial", { "shell", "echo hi" });
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_ANDROID_ADB_CHECK(threw);
    }

    void testThreadCountDefaultsToAtLeastOne()
    {
        AdbCommandExecutor executor("dsbox_definitely_not_a_real_adb_xyz");
        DSBOX_ANDROID_ADB_CHECK(executor.threadCount() >= static_cast<std::size_t>(1));
    }

    void testThreadCountHonorsExplicitValue()
    {
        AdbCommandExecutor executor("dsbox_definitely_not_a_real_adb_xyz", 3);
        DSBOX_ANDROID_ADB_CHECK_EQ(executor.threadCount(), static_cast<std::size_t>(3));
    }

    // parallelForEach() is the piece that stands in for "a threadpool
    // for parallel adb subprocess work" -- exercised here with plain
    // in-memory work (no real adb involved) so it can run anywhere.

    void testParallelForEachPreservesOrderAndRunsEveryItem()
    {
        AdbCommandExecutor executor("dsbox_definitely_not_a_real_adb_xyz", 4);

        std::vector<int> items{ 1, 2, 3, 4, 5 };
        std::atomic<int> callCount{ 0 };

        auto results = executor.parallelForEach(items, [&](const int& item)
        {
            ++callCount;
            return item * 10;
        });

        DSBOX_ANDROID_ADB_CHECK_EQ(callCount.load(), 5);
        DSBOX_ANDROID_ADB_CHECK_EQ(results.size(), static_cast<std::size_t>(5));

        bool inOrder = true;
        for (std::size_t i = 0; i < results.size(); ++i)
            if (results[i] != static_cast<int>(items[i]) * 10)
                inOrder = false;

        DSBOX_ANDROID_ADB_CHECK(inOrder);
    }

    void testParallelForEachPropagatesException()
    {
        AdbCommandExecutor executor("dsbox_definitely_not_a_real_adb_xyz", 4);

        std::vector<int> items{ 1, 2, 3 };

        bool threw = false;
        try
        {
            executor.parallelForEach(items, [](const int& item) -> int
            {
                if (item == 2)
                    throw std::runtime_error("boom");
                return item;
            });
        }
        catch (const std::runtime_error& error)
        {
            threw = true;
            DSBOX_ANDROID_ADB_CHECK_EQ(std::string(error.what()), std::string("boom"));
        }

        DSBOX_ANDROID_ADB_CHECK(threw);
    }

    void testParallelForEachOnEmptyItemsReturnsEmpty()
    {
        AdbCommandExecutor executor("dsbox_definitely_not_a_real_adb_xyz", 2);

        std::vector<int> items;
        auto results = executor.parallelForEach(items, [](const int& item) { return item; });

        DSBOX_ANDROID_ADB_CHECK(results.empty());
    }

    /*
     * AdbDevice / AdbShell / AdbPackages -- construction and wiring,
     * without spawning adb
     */

    void testAdbDeviceRejectsNullExecutor()
    {
        bool threw = false;
        try
        {
            AdbDevice device(nullptr, "some-serial");
            (void)device;
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }

        DSBOX_ANDROID_ADB_CHECK(threw);
    }

    void testAdbDeviceExposesSerialAndExecutor()
    {
        auto executor = std::make_shared<AdbCommandExecutor>("dsbox_definitely_not_a_real_adb_xyz");
        AdbDevice device(executor, "emulator-5554");

        DSBOX_ANDROID_ADB_CHECK_EQ(device.serial(), std::string("emulator-5554"));
        DSBOX_ANDROID_ADB_CHECK(device.executor() == executor);
    }

    void testGetShellAndGetPackagesShareTheSameExecutorAndSerial()
    {
        auto executor = std::make_shared<AdbCommandExecutor>("dsbox_definitely_not_a_real_adb_xyz");
        AdbDevice device(executor, "emulator-5554");

        AdbShell shell = device.getShell();
        AdbPackages packages = device.getPackages();

        DSBOX_ANDROID_ADB_CHECK_EQ(shell.serial(), std::string("emulator-5554"));
        DSBOX_ANDROID_ADB_CHECK_EQ(packages.serial(), std::string("emulator-5554"));
        DSBOX_ANDROID_ADB_CHECK(shell.executor() == executor);
        DSBOX_ANDROID_ADB_CHECK(packages.executor() == executor);
    }

    void testDeviceCommandsThrowForMissingAdbExecutable()
    {
        auto executor = std::make_shared<AdbCommandExecutor>("dsbox_definitely_not_a_real_adb_xyz");
        AdbDevice device(executor, "some-serial");

        bool threw = false;
        try
        {
            device.shell("echo hi");
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_ANDROID_ADB_CHECK(threw);
    }
}

void runAdbDeviceTests()
{
    runTest("DefaultAdbOptionsIncludesCommandThreads", testDefaultAdbOptionsIncludesCommandThreads);
    runTest("RunThrowsForMissingAdbExecutable", testRunThrowsForMissingAdbExecutable);
    runTest("ThreadCountDefaultsToAtLeastOne", testThreadCountDefaultsToAtLeastOne);
    runTest("ThreadCountHonorsExplicitValue", testThreadCountHonorsExplicitValue);
    runTest("ParallelForEachPreservesOrderAndRunsEveryItem", testParallelForEachPreservesOrderAndRunsEveryItem);
    runTest("ParallelForEachPropagatesException", testParallelForEachPropagatesException);
    runTest("ParallelForEachOnEmptyItemsReturnsEmpty", testParallelForEachOnEmptyItemsReturnsEmpty);

    runTest("AdbDeviceRejectsNullExecutor", testAdbDeviceRejectsNullExecutor);
    runTest("AdbDeviceExposesSerialAndExecutor", testAdbDeviceExposesSerialAndExecutor);
    runTest("GetShellAndGetPackagesShareTheSameExecutorAndSerial", testGetShellAndGetPackagesShareTheSameExecutorAndSerial);
    runTest("DeviceCommandsThrowForMissingAdbExecutable", testDeviceCommandsThrowForMissingAdbExecutable);
}
