#include <dsbox_android_adb/adb_device.hpp>

#include <stdexcept>
#include <utility>

namespace dsbox_android_adb
{
    namespace
    {
        std::string stripTrailingNewline(std::string text)
        {
            if (!text.empty() && text.back() == '\n')
                text.pop_back();

            if (!text.empty() && text.back() == '\r')
                text.pop_back();

            return text;
        }
    }

    AdbDevice::AdbDevice(std::shared_ptr<AdbCommandExecutor> executor, std::string serial)
        : m_executor(std::move(executor)), m_serial(std::move(serial))
    {
        if (!m_executor)
            throw std::invalid_argument("dsbox_android_adb: AdbDevice requires a non-null AdbCommandExecutor");
    }

    const std::string& AdbDevice::serial() const noexcept
    {
        return m_serial;
    }

    AdbCommandResult AdbDevice::run(std::vector<std::string> arguments) const
    {
        return m_executor->run(m_serial, std::move(arguments));
    }

    std::string AdbDevice::shell(std::string cmd) const
    {
        return stripTrailingNewline(run({ "shell", std::move(cmd) }).stdoutText);
    }

    AdbCommandResult AdbDevice::emu(std::vector<std::string> command) const
    {
        std::vector<std::string> arguments{ "emu" };
        arguments.insert(arguments.end(), std::make_move_iterator(command.begin()), std::make_move_iterator(command.end()));
        return run(std::move(arguments));
    }

    AdbCommandResult AdbDevice::install(std::string apkPath, std::vector<std::string> extraArgs) const
    {
        std::vector<std::string> arguments{ "install" };
        arguments.insert(arguments.end(), std::make_move_iterator(extraArgs.begin()), std::make_move_iterator(extraArgs.end()));
        arguments.push_back(std::move(apkPath));
        return run(std::move(arguments));
    }

    AdbCommandResult AdbDevice::uninstall(std::string packageName) const
    {
        return run({ "uninstall", std::move(packageName) });
    }

    AdbCommandResult AdbDevice::push(std::string localPath, std::string remotePath) const
    {
        return run({ "push", std::move(localPath), std::move(remotePath) });
    }

    AdbCommandResult AdbDevice::pull(std::string remotePath, std::string localPath) const
    {
        return run({ "pull", std::move(remotePath), std::move(localPath) });
    }

    AdbCommandResult AdbDevice::reboot(std::string mode) const
    {
        std::vector<std::string> arguments{ "reboot" };
        if (!mode.empty())
            arguments.push_back(std::move(mode));

        return run(std::move(arguments));
    }

    AdbShell AdbDevice::getShell() const
    {
        return AdbShell(m_executor, m_serial);
    }

    AdbPackages AdbDevice::getPackages() const
    {
        return AdbPackages(m_executor, m_serial);
    }

    const std::shared_ptr<AdbCommandExecutor>& AdbDevice::executor() const noexcept
    {
        return m_executor;
    }
}
