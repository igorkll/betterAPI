#pragma once

#include <dsbox_android_adb/api.hpp>
#include <dsbox_android_adb/command_executor.hpp>

#include <memory>
#include <string>
#include <vector>

namespace dsbox_android_adb
{
    // A serial-bound wrapper around `adb shell`. Obtained from
    // AdbDevice::getShell() -- cheap to construct directly too, see the
    // constructor below. Every call here blocks the calling thread until
    // the shell command has exited; see AdbCommandExecutor::parallelForEach()
    // for running several at once.
    class DSBOX_ANDROID_ADB_API AdbShell
    {
    public:
        AdbShell(std::shared_ptr<AdbCommandExecutor> executor, std::string serial);

        const std::string& serial() const noexcept;

        // `adb -s <serial> shell <cmd>`, full result (stdout, stderr --
        // adb shell folds the remote command's own stderr into this
        // same stream, so there is rarely anything useful on adb's own
        // stderr here -- and exit code, which on recent adb/Android is
        // the REMOTE command's exit status, not just adb's own).
        AdbCommandResult runRaw(std::string cmd) const;

        // Convenience over runRaw(): returns stdout with a single
        // trailing newline stripped if present (adb shell always adds
        // one), regardless of the remote command's exit code. Use
        // runRaw() if you need to tell a failing command apart from one
        // that succeeded with empty output.
        std::string run(std::string cmd) const;

        // `[ -e <path> ]` via run() -- true if the path exists on the
        // device (file, directory, symlink target or not, anything
        // `test -e` accepts).
        bool exists(std::string path) const;

        // `ls -1 <path>`, split into lines (empty lines dropped). Throws
        // nothing of its own for a missing path -- ls's error text ends
        // up as the (likely single) "line" returned, same as a human
        // would see running it by hand; check exists() first if that
        // distinction matters to the caller.
        std::vector<std::string> listDir(std::string path) const;

        // `getprop <key>` with the trailing newline stripped. Empty
        // string for an unset property, same as adb itself prints.
        std::string getProp(std::string key) const;

        const std::shared_ptr<AdbCommandExecutor>& executor() const noexcept;

    private:
        std::shared_ptr<AdbCommandExecutor> m_executor;
        std::string m_serial;
    };
}
