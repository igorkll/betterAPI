#pragma once

#include <dsbox_android_adb/adb_packages.hpp>
#include <dsbox_android_adb/adb_shell.hpp>
#include <dsbox_android_adb/api.hpp>
#include <dsbox_android_adb/command_executor.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dsbox_android_adb
{
    // A convenient, serial-bound handle for driving ONE Android device
    // through `adb`. Obtained from AdbModule::device(serial) (or
    // constructed directly, see below) -- cheap to copy (a shared_ptr to
    // the underlying AdbCommandExecutor plus a std::string serial) and
    // safe to keep around past the AdbModule that produced it, past a
    // device disconnecting and reconnecting, and past a refreshDevices()
    // that no longer lists this serial: every method here just runs
    // `adb -s <serial> ...` and reports whatever adb itself says, same
    // as typing it by hand -- this class never re-validates the serial
    // against any snapshot.
    //
    // Every method blocks the calling thread until the underlying `adb`
    // subprocess exits -- there is no async variant. Driving several
    // devices (or firing several independent commands at the SAME
    // device) at once is a matter of calling these methods from several
    // threads yourself, or using executor()->parallelForEach() /
    // AdbModule::forEachDevice() with a lambda that calls into this
    // class -- both run on this handle's own AdbCommandExecutor pool, so
    // the calls that actually matter (spawning N `adb` subprocesses)
    // genuinely overlap instead of queuing behind each other.
    //
    // Fully standalone: `AdbDevice(std::make_shared<AdbCommandExecutor>(), "emulator-5554")`
    // works with no dsbox::Context, no IModule, no AdbModule at all --
    // useful in a unit test or a tool with no module system.
    class DSBOX_ANDROID_ADB_API AdbDevice
    {
    public:
        AdbDevice(std::shared_ptr<AdbCommandExecutor> executor, std::string serial);

        const std::string& serial() const noexcept;

        // The building block every other method here is written in
        // terms of: `adb -s <serial> <arguments...>` to completion.
        AdbCommandResult run(std::vector<std::string> arguments) const;

        // `adb -s <serial> shell <cmd>`. Returns stdout with a single
        // trailing newline stripped (adb shell always adds one) --
        // everything else is left exactly as the device printed it.
        // Equivalent to getShell().run(cmd); use run({"shell", cmd}) or
        // getShell().runRaw(cmd) if you need the untouched output, exit
        // code, or stderr.
        std::string shell(std::string cmd) const;

        // `adb -s <serial> emu <command...>` -- the emulator console
        // protocol. Only meaningful against an emulator; adb reports an
        // error (nonzero exit, text on stderr) against a physical
        // device.
        AdbCommandResult emu(std::vector<std::string> command) const;

        // `adb -s <serial> install [extraArgs...] <apkPath>`.
        AdbCommandResult install(std::string apkPath, std::vector<std::string> extraArgs = {}) const;

        // `adb -s <serial> uninstall <packageName>`. Equivalent to
        // getPackages().uninstall(packageName).
        AdbCommandResult uninstall(std::string packageName) const;

        AdbCommandResult push(std::string localPath, std::string remotePath) const;
        AdbCommandResult pull(std::string remotePath, std::string localPath) const;

        // `adb -s <serial> reboot [mode]` -- `mode` empty (the default)
        // is a normal reboot; "bootloader"/"recovery"/"sideload"/
        // "sideload-auto-reboot" match what `adb reboot` itself accepts.
        AdbCommandResult reboot(std::string mode = {}) const;

        // Specialized wrappers bound to this same serial/executor. Each
        // call constructs a small, cheap, independent handle -- there is
        // no cache, so getShell() called twice returns two distinct (but
        // interchangeable) AdbShell instances.
        AdbShell getShell() const;
        AdbPackages getPackages() const;

        // The shared backend this handle runs on -- use
        // executor()->parallelForEach() to drive several devices (or
        // several independent commands on this one) concurrently, or
        // executor()->run()/post() directly for lower-level access.
        const std::shared_ptr<AdbCommandExecutor>& executor() const noexcept;

    private:
        std::shared_ptr<AdbCommandExecutor> m_executor;
        std::string m_serial;
    };
}
