#pragma once

#include <string>
#include <vector>

namespace dsbox_android_adb
{
    struct AdbDeviceInfo;
    struct AdbCommandResult;
    class AdbModule;
}

namespace dsbox_adb_tool
{
    // Used by the GUI panels (gui_mode.cpp), so a device table or a command's
    // exit-code/stdout/stderr looks the same whichever front end is
    // driving AdbModule.

    std::string formatDeviceLine(const dsbox_android_adb::AdbDeviceInfo& device);
    std::string formatDevicesTable(const std::vector<dsbox_android_adb::AdbDeviceInfo>& devices);

    // Full exit code + stdout + stderr of one AdbCommandResult, for
    // commands (install/uninstall/push/pull/reboot) where adb itself
    // decides success/failure and often says more on stderr than a bare
    // exit code conveys.
    std::string formatCommandResult(const dsbox_android_adb::AdbCommandResult& result);

    // AdbModule never validates a serial against the live snapshot (by
    // design), so a typo just silently drives a nonexistent device.
    // Returns a one-line warning if `serial` isn't currently known,
    // empty otherwise.
    std::string unknownSerialWarning(const dsbox_android_adb::AdbModule& adb, const std::string& serial);
}
