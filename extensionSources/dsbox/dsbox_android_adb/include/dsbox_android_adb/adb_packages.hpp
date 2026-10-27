#pragma once

#include <dsbox_android_adb/adb_shell.hpp>
#include <dsbox_android_adb/api.hpp>
#include <dsbox_android_adb/command_executor.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_android_adb
{
    // One line of `cmd appops get <pkg>` / what setAppOp() changes.
    // `mode` is whatever adb prints verbatim ("allow", "ignore", "deny",
    // "default", ...) -- not modeled as an enum since appops has grown
    // more modes across Android versions than are worth hard-coding
    // here.
    struct AppOpEntry
    {
        std::string op;
        std::string mode;
    };

    // One entry of `dumpsys package <pkg>`'s permissions listing.
    // `granted` reflects install-time permissions as dumpsys reports
    // them; for a runtime permission this is "has the user granted it",
    // same meaning a human reads off `dumpsys package` by hand.
    struct PackageDumpPermission
    {
        std::string name;
        bool granted = false;
    };

    // A serial-bound wrapper around the handful of `adb shell pm` /
    // `dumpsys package` / `cmd appops` / `am` invocations that come up
    // constantly when scripting against an app on a device: listing and
    // inspecting packages, granting/revoking permissions, poking appops,
    // flipping compat-framework flags, clearing data, force-stopping,
    // starting an activity. Obtained from AdbDevice::getPackages().
    //
    // This class does its best to parse `dumpsys`/`pm`/`cmd` text output
    // -- there is no stable machine-readable format for most of this on
    // stock Android, so parsing here is necessarily best-effort and
    // tolerant of the kind of formatting drift dumpsys has shown across
    // versions (same spirit as parseAdbDevicesOutput()). Where an
    // operation's success can't be inferred reliably from adb's own exit
    // code (pm/am/cmd frequently print a human-readable failure message
    // while still exiting 0 through `adb shell`), the raw
    // AdbCommandResult is returned instead of a guessed bool, so the
    // caller can inspect stdout/stderr itself rather than trust a
    // heuristic that might be wrong for their Android version.
    class DSBOX_ANDROID_ADB_API AdbPackages
    {
    public:
        AdbPackages(std::shared_ptr<AdbCommandExecutor> executor, std::string serial);

        const std::string& serial() const noexcept;

        // `pm list packages [filter]` -- every installed package whose
        // name contains `filter` (substring match, exactly like `pm`
        // itself), or every package if `filter` is empty. Names come
        // back with the "package:" prefix `pm` prints stripped off.
        std::vector<std::string> list(std::string_view filter = {}) const;

        // list(packageName) with an exact-name check -- `pm`'s own
        // filter is substring, so this is NOT the same as
        // `!list(packageName).empty()` for a name that happens to be a
        // prefix/substring of another installed package.
        bool isInstalled(std::string_view packageName) const;

        // `pm grant <packageName> <permission>` / `pm revoke ...`.
        AdbCommandResult grantPermission(std::string_view packageName, std::string_view permission) const;
        AdbCommandResult revokePermission(std::string_view packageName, std::string_view permission) const;

        // Best-effort parse of the permissions `dumpsys package
        // <packageName>` lists (both the "runtime permissions:" and
        // "install permissions:" sections, wherever a "<name>: granted="
        // shaped line appears). Empty if the package isn't found or
        // dumpsys's format doesn't match what this parser expects --
        // call dumpPackage() directly if you need the raw text to parse
        // yourself.
        std::vector<PackageDumpPermission> permissions(std::string_view packageName) const;

        // `adb shell cmd appops set <packageName> <op> <mode>`.
        AdbCommandResult setAppOp(std::string_view packageName, std::string_view op, std::string_view mode) const;

        // Best-effort parse of `adb shell cmd appops get <packageName>`
        // -- one AppOpEntry per "<op> (...): mode=<mode>..."-shaped
        // line. Empty if appops printed nothing recognizable as that
        // (e.g. "No operations." when every op is at its default).
        std::vector<AppOpEntry> appOps(std::string_view packageName) const;

        // `adb shell am compat enable|disable <changeId> <packageName>`,
        // where `changeId` is either a platform compat-change ID (a
        // number) or its symbolic name, same as `am compat` itself
        // accepts.
        AdbCommandResult setCompatFlag(std::string_view packageName, std::string_view changeId, bool enabled) const;

        AdbCommandResult clearData(std::string_view packageName) const;   // `pm clear <packageName>`
        AdbCommandResult forceStop(std::string_view packageName) const;   // `am force-stop <packageName>`
        AdbCommandResult uninstall(std::string_view packageName) const;   // `pm uninstall <packageName>`

        // `am start -n <component> [extraArgs...]`. `component` is
        // "package/.ActivityClass" or "package/full.ClassName", exactly
        // as `am start -n` itself expects.
        AdbCommandResult startActivity(std::string_view component, std::vector<std::string> extraArgs = {}) const;

        // The raw `dumpsys package <packageName>` text, for anything the
        // parsed helpers above don't cover.
        std::string dumpPackage(std::string_view packageName) const;

        const std::shared_ptr<AdbCommandExecutor>& executor() const noexcept;

    private:
        AdbShell m_shell;
    };
}
