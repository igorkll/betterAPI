#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace dsbox_android_adb
{
    // Mirrors the second column of `adb devices -l`. Unknown covers any
    // token this enum doesn't name yet (adb has grown new ones over the
    // years, e.g. "authorizing") -- AdbDeviceInfo::rawState always
    // carries the original text regardless of how it maps here, so
    // nothing is lost.
    enum class AdbDeviceState
    {
        Device,        // "device"            -- ready to use
        Offline,       // "offline"
        Unauthorized,  // "unauthorized"       -- waiting on the on-device prompt
        NoPermissions, // "no" "permissions"   -- udev/driver issue (desktop only)
        Bootloader,    // "bootloader"         -- fastboot mode, not adb
        Recovery,      // "recovery"
        Unknown
    };

    // One row of `adb devices -l` -- a snapshot, not a live handle. Plain
    // data -- kept header-only so it can cross the DSBoxAndroidAdb.dll
    // boundary without its own export macro, the same convention as
    // dsbox_config::ModuleLoadResult.
    //
    // This is what refreshDevices()/devices() and the "adb.device.*"
    // events traffic in. It does NOT know how to run commands against
    // the device it describes -- for that, turn a serial into a live
    // dsbox_android_adb::AdbDevice handle via AdbModule::device() (or
    // AdbCommandExecutor directly), e.g.
    // `module.device(info.serial).shell("echo hi")`.
    struct AdbDeviceInfo
    {
        std::string serial;
        AdbDeviceState state = AdbDeviceState::Unknown;
        std::string rawState; // the state token(s) exactly as adb printed them

        // Every "key:value" token after the state column -- typically
        // product, model, device and transport_id, sometimes usb. Empty
        // for a device adb only reports a serial and state for.
        std::unordered_map<std::string, std::string> properties;

        std::string property(const std::string& key, std::string fallback = {}) const
        {
            const auto it = properties.find(key);
            return it != properties.end() ? it->second : fallback;
        }
    };

    inline const char* toString(AdbDeviceState state) noexcept
    {
        switch (state)
        {
            case AdbDeviceState::Device: return "device";
            case AdbDeviceState::Offline: return "offline";
            case AdbDeviceState::Unauthorized: return "unauthorized";
            case AdbDeviceState::NoPermissions: return "no permissions";
            case AdbDeviceState::Bootloader: return "bootloader";
            case AdbDeviceState::Recovery: return "recovery";
            case AdbDeviceState::Unknown: default: return "unknown";
        }
    }

    // Reported by AdbModule::refreshDevices() and the periodic device
    // watch: the full current snapshot plus what changed since the
    // previous one. `devices` is already what AdbModule::devices() will
    // return right after this fires.
    struct DevicesChangedEvent
    {
        std::vector<AdbDeviceInfo> devices;
        std::vector<AdbDeviceInfo> added;
        std::vector<std::string> removedSerials;
    };

    // Payload of "adb.logcat.line" (stdout -- the actual log output) and
    // "adb.logcat.stderr_line" (stderr -- adb's own diagnostics, e.g.
    // "error: device 'XYZ' not found" when `serial` doesn't match a
    // connected device). Same struct, two topics: a consumer that only
    // cares about log lines can subscribe to "adb.logcat.line" alone,
    // but subscribing to "adb.logcat.stderr_line" too is strongly
    // recommended -- without it, adb failures (bad serial, unauthorized
    // device, logcat crashing) are invisible beyond
    // LogcatStoppedEvent::exitCode being nonzero.
    struct LogcatLineEvent
    {
        std::string serial;
        std::string line;
    };

    // Payload of "adb.logcat.stopped": the logcat subprocess for `serial`
    // is no longer running, whichever side ended it (stopLogcat(),
    // process exit, or AdbModule shutdown).
    struct LogcatStoppedEvent
    {
        std::string serial;
        int exitCode = 0;
    };
}
