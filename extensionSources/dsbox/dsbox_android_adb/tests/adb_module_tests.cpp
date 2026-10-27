#include "test_framework.hpp"

#include <dsbox_android_adb/adb_module.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

using namespace dsbox_android_adb_tests;
using namespace dsbox_android_adb;

namespace
{
    const AdbDeviceInfo* findBySerial(const std::vector<AdbDeviceInfo>& devices, const std::string& serial)
    {
        for (const auto& device : devices)
            if (device.serial == serial)
                return &device;

        return nullptr;
    }

    /*
     * parseAdbDevicesOutput()
     */

    void testParsesHeaderAndEmptyOutput()
    {
        const auto devices = parseAdbDevicesOutput("List of devices attached\n\n");
        DSBOX_ANDROID_ADB_CHECK(devices.empty());
    }

    void testParsesEmptyString()
    {
        const auto devices = parseAdbDevicesOutput("");
        DSBOX_ANDROID_ADB_CHECK(devices.empty());
    }

    void testParsesSingleDeviceWithProperties()
    {
        const auto devices = parseAdbDevicesOutput(
            "List of devices attached\n"
            "emulator-5554          device product:sdk_gphone64_x86_64 model:sdk_gphone64_x86_64 device:emulator64_x86_64_arm64 transport_id:1\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));

        const AdbDeviceInfo* device = findBySerial(devices, "emulator-5554");
        DSBOX_ANDROID_ADB_CHECK(device != nullptr);
        if (!device)
            return;

        DSBOX_ANDROID_ADB_CHECK(device->state == AdbDeviceState::Device);
        DSBOX_ANDROID_ADB_CHECK_EQ(device->rawState, std::string("device"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("product"), std::string("sdk_gphone64_x86_64"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("model"), std::string("sdk_gphone64_x86_64"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("device"), std::string("emulator64_x86_64_arm64"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("transport_id"), std::string("1"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("missing", "fallback"), std::string("fallback"));
    }

    void testParsesWithoutHeaderLine()
    {
        const auto devices = parseAdbDevicesOutput("R58N90ABCDE             unauthorized usb:1-1 transport_id:2\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));

        const AdbDeviceInfo* device = findBySerial(devices, "R58N90ABCDE");
        DSBOX_ANDROID_ADB_CHECK(device != nullptr);
        if (!device)
            return;

        DSBOX_ANDROID_ADB_CHECK(device->state == AdbDeviceState::Unauthorized);
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("usb"), std::string("1-1"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("transport_id"), std::string("2"));
    }

    void testParsesNoPermissionsState()
    {
        const auto devices = parseAdbDevicesOutput("SERIAL123 no permissions usb:1-1\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));

        const AdbDeviceInfo* device = findBySerial(devices, "SERIAL123");
        DSBOX_ANDROID_ADB_CHECK(device != nullptr);
        if (!device)
            return;

        DSBOX_ANDROID_ADB_CHECK(device->state == AdbDeviceState::NoPermissions);
        DSBOX_ANDROID_ADB_CHECK_EQ(device->rawState, std::string("no permissions"));
        DSBOX_ANDROID_ADB_CHECK_EQ(device->property("usb"), std::string("1-1"));
    }

    void testParsesDeviceWithNoProperties()
    {
        const auto devices = parseAdbDevicesOutput("List of devices attached\nSERIALXYZ device\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));

        const AdbDeviceInfo* device = findBySerial(devices, "SERIALXYZ");
        DSBOX_ANDROID_ADB_CHECK(device != nullptr);
        if (!device)
            return;

        DSBOX_ANDROID_ADB_CHECK(device->state == AdbDeviceState::Device);
        DSBOX_ANDROID_ADB_CHECK(device->properties.empty());
    }

    void testIgnoresBlankAndWhitespaceLines()
    {
        const auto devices = parseAdbDevicesOutput(
            "List of devices attached\n"
            "\n"
            "   \n"
            "emulator-5554 device\n"
            "\r\n"
            "emulator-5556 offline\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(2));
        DSBOX_ANDROID_ADB_CHECK(findBySerial(devices, "emulator-5554") != nullptr);

        const AdbDeviceInfo* offlineDevice = findBySerial(devices, "emulator-5556");
        DSBOX_ANDROID_ADB_CHECK(offlineDevice != nullptr);
        if (offlineDevice)
            DSBOX_ANDROID_ADB_CHECK(offlineDevice->state == AdbDeviceState::Offline);
    }

    void testParsesMultipleDevices()
    {
        const auto devices = parseAdbDevicesOutput(
            "List of devices attached\n"
            "emulator-5554 device product:sdk_gphone64_x86_64\n"
            "0123456789ABCDEF device product:raven model:Pixel_6_Pro\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(2));
        DSBOX_ANDROID_ADB_CHECK(findBySerial(devices, "emulator-5554") != nullptr);
        DSBOX_ANDROID_ADB_CHECK(findBySerial(devices, "0123456789ABCDEF") != nullptr);
    }

    void testUnknownStateIsPreservedAsRawState()
    {
        const auto devices = parseAdbDevicesOutput("SERIALABC authorizing\n");

        DSBOX_ANDROID_ADB_CHECK_EQ(devices.size(), static_cast<std::size_t>(1));
        DSBOX_ANDROID_ADB_CHECK(devices[0].state == AdbDeviceState::Unknown);
        DSBOX_ANDROID_ADB_CHECK_EQ(devices[0].rawState, std::string("authorizing"));
    }

    /*
     * AdbModule -- behavior that doesn't require a real `adb` on PATH
     */

    void testDefaultOptions()
    {
        AdbOptions options;
        DSBOX_ANDROID_ADB_CHECK_EQ(options.adbPath, std::string("adb"));
        DSBOX_ANDROID_ADB_CHECK(options.autoStartDeviceWatch);
        DSBOX_ANDROID_ADB_CHECK(options.deviceWatchInterval.count() > 0);
    }

    void testModuleNameAndEmptyInitialState()
    {
        AdbModule module;

        DSBOX_ANDROID_ADB_CHECK_EQ(std::string(module.name()), std::string("dsbox.android.adb"));
        DSBOX_ANDROID_ADB_CHECK(module.devices().empty());
        DSBOX_ANDROID_ADB_CHECK(!module.isWatchingDevices());
        DSBOX_ANDROID_ADB_CHECK(module.activeLogcatSerials().empty());
        DSBOX_ANDROID_ADB_CHECK(!module.isLogcatRunning("whatever-serial"));
    }

    void testStartDeviceWatchRejectsNonPositiveInterval()
    {
        AdbModule module;

        bool threwForZero = false;
        try
        {
            module.startDeviceWatch(std::chrono::milliseconds(0));
        }
        catch (const std::invalid_argument&)
        {
            threwForZero = true;
        }
        DSBOX_ANDROID_ADB_CHECK(threwForZero);

        bool threwForNegative = false;
        try
        {
            module.startDeviceWatch(std::chrono::milliseconds(-100));
        }
        catch (const std::invalid_argument&)
        {
            threwForNegative = true;
        }
        DSBOX_ANDROID_ADB_CHECK(threwForNegative);

        DSBOX_ANDROID_ADB_CHECK(!module.isWatchingDevices());
    }

    void testStartAndStopFallbackDeviceWatch()
    {
        // No dsbox::Context/onInit() here, so startDeviceWatch() has no
        // Scheduler to find and falls back to its private timer thread --
        // this exercises that path without needing a real adb on PATH
        // (the interval is long enough that no tick should fire before
        // stopDeviceWatch() cancels it).
        AdbModule module;

        module.startDeviceWatch(std::chrono::milliseconds(60'000));
        DSBOX_ANDROID_ADB_CHECK(module.isWatchingDevices());

        // Starting again while already watching is a documented no-op.
        module.startDeviceWatch(std::chrono::milliseconds(1));
        DSBOX_ANDROID_ADB_CHECK(module.isWatchingDevices());

        module.stopDeviceWatch();
        DSBOX_ANDROID_ADB_CHECK(!module.isWatchingDevices());

        // Stopping an already-stopped watch is a no-op, not an error.
        module.stopDeviceWatch();
        DSBOX_ANDROID_ADB_CHECK(!module.isWatchingDevices());
    }

    void testRefreshDevicesThrowsForMissingAdbExecutable()
    {
        AdbOptions options;
        options.adbPath = "dsbox_definitely_not_a_real_adb_xyz";

        AdbModule module(options);

        bool threw = false;
        try
        {
            module.refreshDevices();
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_ANDROID_ADB_CHECK(threw);
        DSBOX_ANDROID_ADB_CHECK(module.devices().empty());
    }

    void testStartLogcatThrowsForMissingAdbExecutable()
    {
        AdbOptions options;
        options.adbPath = "dsbox_definitely_not_a_real_adb_xyz";

        AdbModule module(options);

        bool threw = false;
        try
        {
            module.startLogcat("some-serial");
        }
        catch (const std::exception&)
        {
            threw = true;
        }

        DSBOX_ANDROID_ADB_CHECK(threw);
        DSBOX_ANDROID_ADB_CHECK(!module.isLogcatRunning("some-serial"));
        DSBOX_ANDROID_ADB_CHECK(module.activeLogcatSerials().empty());
    }

    void testStopLogcatIsNoOpWhenNotRunning()
    {
        AdbModule module;

        // Must not throw.
        module.stopLogcat("never-started-serial");
        DSBOX_ANDROID_ADB_CHECK(!module.isLogcatRunning("never-started-serial"));
    }
}

void runAdbModuleTests()
{
    runTest("ParsesHeaderAndEmptyOutput", testParsesHeaderAndEmptyOutput);
    runTest("ParsesEmptyString", testParsesEmptyString);
    runTest("ParsesSingleDeviceWithProperties", testParsesSingleDeviceWithProperties);
    runTest("ParsesWithoutHeaderLine", testParsesWithoutHeaderLine);
    runTest("ParsesNoPermissionsState", testParsesNoPermissionsState);
    runTest("ParsesDeviceWithNoProperties", testParsesDeviceWithNoProperties);
    runTest("IgnoresBlankAndWhitespaceLines", testIgnoresBlankAndWhitespaceLines);
    runTest("ParsesMultipleDevices", testParsesMultipleDevices);
    runTest("UnknownStateIsPreservedAsRawState", testUnknownStateIsPreservedAsRawState);

    runTest("DefaultOptions", testDefaultOptions);
    runTest("ModuleNameAndEmptyInitialState", testModuleNameAndEmptyInitialState);
    runTest("StartDeviceWatchRejectsNonPositiveInterval", testStartDeviceWatchRejectsNonPositiveInterval);
    runTest("StartAndStopFallbackDeviceWatch", testStartAndStopFallbackDeviceWatch);
    runTest("RefreshDevicesThrowsForMissingAdbExecutable", testRefreshDevicesThrowsForMissingAdbExecutable);
    runTest("StartLogcatThrowsForMissingAdbExecutable", testStartLogcatThrowsForMissingAdbExecutable);
    runTest("StopLogcatIsNoOpWhenNotRunning", testStopLogcatIsNoOpWhenNotRunning);
}
