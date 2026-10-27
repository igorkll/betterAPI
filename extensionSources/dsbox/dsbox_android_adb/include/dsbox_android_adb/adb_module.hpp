#pragma once

#include <dsbox_android_adb/adb_device.hpp>
#include <dsbox_android_adb/api.hpp>
#include <dsbox_android_adb/command_executor.hpp>
#include <dsbox_android_adb/device.hpp>

#include <dsbox/module.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dsbox_android_adb
{
    // Configuration for a single AdbModule instance, fixed for its whole
    // lifetime (no setters -- construct a new instance if you need
    // different behavior). onInit() overrides every field below from the
    // [android.adb] section of a dsbox_config::PersistentConfigStore, if
    // one has been provide()'d -- same convention as
    // dsbox_net_socket::SocketModule.
    //
    //   [android.adb]
    //   adb_path                  = adb       ; executable name/path
    //   device_watch_interval_ms  = 1000       ; 0 disables the periodic watch
    //   auto_start_device_watch   = true
    //   command_threads           = 0          ; 0 = hardware_concurrency(), see AdbCommandExecutor
    struct AdbOptions
    {
        // Passed as SubprocessOptions::executable for every `adb`
        // invocation. A bare name ("adb", the default) is resolved via
        // PATH; a full path is used as-is -- see
        // dsbox_process::SubprocessOptions.
        std::string adbPath = "adb";

        // Interval for the periodic background refreshDevices() started
        // automatically at onStart() when autoStartDeviceWatch is true.
        // Also the interval used by a later startDeviceWatch() call made
        // with no explicit interval. Must be > 0 for the automatic watch
        // to actually start -- 0 just means "nothing running yet".
        std::chrono::milliseconds deviceWatchInterval{ 1000 };

        // Whether onStart() calls startDeviceWatch(deviceWatchInterval)
        // on its own. Set to false to drive device discovery manually
        // via refreshDevices()/startDeviceWatch().
        bool autoStartDeviceWatch = true;

        // Worker count for the AdbCommandExecutor backing every
        // AdbDevice/AdbShell/AdbPackages handle this module hands out
        // (device(), forEachDevice()) -- how many `adb` subprocesses can
        // genuinely run at once when driving several devices (or several
        // commands) in parallel. 0 (the default) means
        // std::thread::hardware_concurrency(), same convention as
        // dsbox_scheduler::ThreadPoolExecutor. The pool itself only
        // starts on first use, so this costs nothing if nothing ever
        // calls into it.
        std::size_t commandThreads = 0;
    };

    // Drives the `adb` command-line tool as a subprocess (dsbox_process)
    // to discover devices, stream logcat output, and -- via device()/
    // forEachDevice() -- run arbitrary commands (shell, install, package
    // management, ...) against one or more devices, in parallel when
    // there's more than one. This module does not speak the ADB wire
    // protocol itself -- it shells out to whatever `adb` the host has on
    // PATH (or at AdbOptions::adbPath), the same way a human would at a
    // terminal, and parses its text output.
    //
    // Everything this module learns about device connect/disconnect and
    // logcat is also published on the Context's CommunicationManager, so
    // other modules (a CLI command, a UI panel) can stay in sync without
    // holding a direct reference to this one:
    //
    //   topic                      payload
    //   adb.devices.changed        DevicesChangedEvent  (full snapshot + diff)
    //   adb.device.connected       AdbDeviceInfo          (one per newly seen serial)
    //   adb.device.disconnected    AdbDeviceInfo          (one per serial that dropped out)
    //   adb.logcat.line            LogcatLineEvent        (one per logcat STDOUT line)
    //   adb.logcat.stderr_line     LogcatLineEvent        (one per logcat STDERR line --
    //                                                       adb's own diagnostics; see
    //                                                       LogcatLineEvent's own docs for
    //                                                       why this is worth subscribing to)
    //   adb.logcat.stopped         LogcatStoppedEvent
    //
    // Device discovery: refreshDevices() runs `adb devices -l` to
    // completion (blocking the calling thread -- it's a one-shot command,
    // typically done in well under a second) and diffs the result against
    // the previous snapshot. startDeviceWatch() repeats that on a timer,
    // using the Context's shared dsbox_scheduler::Scheduler if one has
    // been published, or a private background thread otherwise -- exactly
    // the optional-scheduler convention documented on
    // dsbox_io::IOModule and dsbox_scheduler::SchedulerModule. Nothing
    // requires the scheduler module to be present.
    //
    // Driving devices: device(serial) hands out an AdbDevice -- a cheap,
    // copyable handle with .shell()/.install()/.emu()/... plus
    // .getShell()/.getPackages() wrappers for the common follow-on
    // scripting (see adb_device.hpp, adb_shell.hpp, adb_packages.hpp).
    // Every handle shares this module's own AdbCommandExecutor
    // (AdbOptions::commandThreads workers), so forEachDevice() (or
    // commandExecutor()->parallelForEach() directly) genuinely runs
    // several `adb` subprocesses at once instead of one after another.
    // A handle keeps working even after this AdbModule is destroyed --
    // see AdbCommandExecutor's own docs.
    //
    // Logcat: startLogcat(serial) spawns a long-running `adb -s <serial>
    // logcat` subprocess whose stdout is re-assembled into lines (adb's
    // own output is not guaranteed to arrive chunk-aligned with line
    // boundaries) and published one "adb.logcat.line" event at a time.
    // Each serial has at most one logcat subprocess at a time; call
    // stopLogcat() or let the device disconnect to end it.
    //
    // Lifecycle: onInit() reads AdbOptions overrides from config (if any).
    // onStart() starts the automatic device watch (if configured).
    // onStop()/onShutdown() stop the device watch and every running
    // logcat subprocess, forcibly terminating any that haven't exited on
    // their own -- see dsbox_process::Subprocess's own shutdown docs,
    // which this inherits. The command executor behind device()/
    // forEachDevice() is unaffected by onStop()/onShutdown(): any
    // AdbDevice handles already handed out keep working, per their own
    // docs on outliving their creator.
    class DSBOX_ANDROID_ADB_API AdbModule final : public dsbox::IModule
    {
    public:
        AdbModule();
        explicit AdbModule(AdbOptions options);
        ~AdbModule() override;

        AdbModule(const AdbModule&) = delete;
        AdbModule& operator=(const AdbModule&) = delete;

        std::string_view name() const override
        {
            return "dsbox.android.adb";
        }

        void onInit(dsbox::Context& context) override;
        void onStart(dsbox::Context& context) override;
        void onStop(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Runs `adb devices -l` to completion, parses it, updates the
        // internal snapshot and emits "adb.devices.changed" plus any
        // "adb.device.connected"/"adb.device.disconnected" the diff
        // produces (even if nothing changed -- "adb.devices.changed"
        // still fires, with empty added/removedSerials, since the
        // snapshot itself is still useful to a listener that only just
        // subscribed). Returns the new snapshot.
        //
        // Throws std::runtime_error if `adb` could not be spawned at all
        // (not found, not executable -- see dsbox_process::Subprocess).
        // A nonzero exit code from `adb devices` itself is not treated as
        // an error: its stdout (if any) is still parsed as usual.
        std::vector<AdbDeviceInfo> refreshDevices();

        // The snapshot as of the last refreshDevices()/watch tick. Empty,
        // not an error, before the first refresh has ever run. Does not
        // itself talk to adb.
        std::vector<AdbDeviceInfo> devices() const;

        // Starts refreshDevices() on a repeating timer. No-op if already
        // watching -- call stopDeviceWatch() first to change the
        // interval. Throws std::invalid_argument if interval <= 0.
        //
        // Errors from an individual refreshDevices() tick (e.g. adb
        // transiently unavailable) are swallowed so the watch keeps
        // running; they are not surfaced as events. Use refreshDevices()
        // directly if you need to observe a single attempt's outcome.
        void startDeviceWatch(std::chrono::milliseconds interval);

        // Convenience overload: uses the interval from this module's
        // AdbOptions (as last constructed/configured).
        void startDeviceWatch();

        // No-op if not currently watching.
        void stopDeviceWatch();

        bool isWatchingDevices() const;

        // Spawns `adb -s <serial> logcat`, plus any extraArgs appended
        // after it (e.g. {"-v", "threadtime"} or {"-b", "crash"}), and
        // starts streaming its stdout as "adb.logcat.line" events for
        // this serial. Throws std::runtime_error if logcat is already
        // running for this exact serial, or if spawning adb failed.
        //
        // `serial` is passed to `adb -s` as-is and is not validated
        // against the current device snapshot -- this works even for a
        // device refreshDevices() hasn't seen yet (e.g. right after
        // connect, before the next watch tick). If `serial` is wrong
        // (typo, or a logcat filterspec like "*:E" passed where a
        // serial belongs), adb itself fails fast and the only signal is
        // the subprocess exiting with a nonzero code shortly after this
        // call returns -- see "adb.logcat.stderr_line" in the class docs
        // for how to see *why*.
        void startLogcat(std::string serial, std::vector<std::string> extraArgs = {});

        // Terminates the logcat subprocess for `serial` (if one is
        // running) and emits "adb.logcat.stopped" for it. No-op if
        // logcat isn't running for this serial.
        void stopLogcat(std::string_view serial);

        bool isLogcatRunning(std::string_view serial) const;

        // Serials with a logcat subprocess currently running, in no
        // particular order.
        std::vector<std::string> activeLogcatSerials() const;

        // A live handle for driving `serial` through adb -- see
        // AdbDevice's own docs for what it can do and how its lifetime
        // relates to this module's. `serial` is not validated against
        // the current device snapshot, same convention as startLogcat().
        AdbDevice device(std::string serial) const;

        // The AdbCommandExecutor every device() handle (and, through it,
        // every AdbShell/AdbPackages) shares. Use
        // commandExecutor()->parallelForEach() directly for a custom
        // batch operation, or just call forEachDevice() below for the
        // common case of "run this against every (or some) known
        // device".
        // Lazily constructs the executor (from AdbOptions::adbPath /
        // commandThreads as of the first call) on first use, so this is
        // not noexcept despite returning a reference.
        const std::shared_ptr<AdbCommandExecutor>& commandExecutor() const;

        // Runs `work(device)` once per device in `serials` -- or, if
        // `serials` is empty (the default), once per serial in the
        // current devices() snapshot -- in parallel on this module's
        // AdbCommandExecutor, and blocks until every one has finished.
        // Results come back in the same order as the serials used.
        // Thin convenience over commandExecutor()->parallelForEach():
        // see its docs for how exceptions from `work` are reported.
        //
        //   auto releases = adb.forEachDevice({},
        //       [](const AdbDevice& device) { return device.getShell().getProp("ro.build.version.release"); });
        template<typename Work>
        auto forEachDevice(std::vector<std::string> serials, Work work) const
            -> std::vector<std::invoke_result_t<Work, const AdbDevice&>>
        {
            if (serials.empty())
                for (const auto& info : devices())
                    serials.push_back(info.serial);

            const auto executor = commandExecutor();

            return executor->parallelForEach(serials,
                [&executor, &work](const std::string& serial)
                {
                    return work(AdbDevice(executor, serial));
                });
        }

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // Parses the text `adb devices -l` prints on stdout into a device
    // list. Exposed separately from AdbModule so the parsing logic can be
    // unit-tested without actually spawning adb. Tolerant of:
    //   - a missing/different "List of devices attached" header line
    //   - trailing blank lines / trailing whitespace
    //   - a state made of more than one word ("no permissions")
    //   - a device line with no "-l" properties at all (just serial + state)
    // Lines this parser cannot make sense of at all (fewer than two
    // whitespace-separated tokens) are skipped rather than thrown on.
    DSBOX_ANDROID_ADB_API std::vector<AdbDeviceInfo> parseAdbDevicesOutput(std::string_view output);
}
