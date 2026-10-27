#include <dsbox_android_adb/adb_packages.hpp>

#include <algorithm>
#include <utility>

namespace dsbox_android_adb
{
    namespace
    {
        std::string quoteForShell(std::string_view text)
        {
            std::string quoted = "'";

            for (char c : text)
            {
                if (c == '\'')
                    quoted += "'\\''";
                else
                    quoted += c;
            }

            quoted += '\'';
            return quoted;
        }

        std::string trim(std::string_view text)
        {
            std::size_t begin = 0;
            while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
                ++begin;

            std::size_t end = text.size();
            while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
                --end;

            return std::string(text.substr(begin, end - begin));
        }

        std::vector<std::string> splitLines(const std::string& text)
        {
            std::vector<std::string> lines;

            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t newline = text.find('\n', start);
                const std::string_view line = (newline == std::string::npos)
                    ? std::string_view(text).substr(start)
                    : std::string_view(text).substr(start, newline - start);

                lines.push_back(trim(line));

                if (newline == std::string::npos)
                    break;

                start = newline + 1;
            }

            return lines;
        }
    }

    AdbPackages::AdbPackages(std::shared_ptr<AdbCommandExecutor> executor, std::string serial)
        : m_shell(std::move(executor), std::move(serial))
    {
    }

    const std::string& AdbPackages::serial() const noexcept
    {
        return m_shell.serial();
    }

    std::vector<std::string> AdbPackages::list(std::string_view filter) const
    {
        std::string cmd = "pm list packages";
        if (!filter.empty())
            cmd += " " + quoteForShell(filter);

        std::vector<std::string> names;

        for (const auto& line : splitLines(m_shell.runRaw(cmd).stdoutText))
        {
            static constexpr std::string_view prefix = "package:";

            if (line.size() > prefix.size() && line.compare(0, prefix.size(), prefix) == 0)
                names.push_back(line.substr(prefix.size()));
        }

        return names;
    }

    bool AdbPackages::isInstalled(std::string_view packageName) const
    {
        const auto names = list(packageName);
        return std::find(names.begin(), names.end(), std::string(packageName)) != names.end();
    }

    AdbCommandResult AdbPackages::grantPermission(std::string_view packageName, std::string_view permission) const
    {
        return m_shell.runRaw("pm grant " + quoteForShell(packageName) + " " + quoteForShell(permission));
    }

    AdbCommandResult AdbPackages::revokePermission(std::string_view packageName, std::string_view permission) const
    {
        return m_shell.runRaw("pm revoke " + quoteForShell(packageName) + " " + quoteForShell(permission));
    }

    std::vector<PackageDumpPermission> AdbPackages::permissions(std::string_view packageName) const
    {
        std::vector<PackageDumpPermission> result;

        static constexpr std::string_view marker = ": granted=";

        for (const auto& rawLine : splitLines(dumpPackage(packageName)))
        {
            const auto pos = rawLine.find(marker);
            if (pos == std::string::npos)
                continue;

            PackageDumpPermission entry;
            entry.name = rawLine.substr(0, pos);

            const std::string_view rest(rawLine.data() + pos + marker.size(), rawLine.size() - pos - marker.size());
            entry.granted = rest.compare(0, 4, "true") == 0;

            result.push_back(std::move(entry));
        }

        return result;
    }

    AdbCommandResult AdbPackages::setAppOp(std::string_view packageName, std::string_view op, std::string_view mode) const
    {
        return m_shell.runRaw(
            "cmd appops set " + quoteForShell(packageName) + " " + quoteForShell(op) + " " + quoteForShell(mode));
    }

    std::vector<AppOpEntry> AdbPackages::appOps(std::string_view packageName) const
    {
        std::vector<AppOpEntry> result;

        for (const auto& line : splitLines(m_shell.runRaw("cmd appops get " + quoteForShell(packageName)).stdoutText))
        {
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            const std::string op = trim(std::string_view(line).substr(0, colon));

            std::string_view rest(line.data() + colon + 1, line.size() - colon - 1);

            const auto semicolon = rest.find(';');
            if (semicolon != std::string_view::npos)
                rest = rest.substr(0, semicolon);

            std::string mode = trim(rest);

            static constexpr std::string_view modePrefix = "mode=";
            if (mode.compare(0, modePrefix.size(), modePrefix) == 0)
                mode = mode.substr(modePrefix.size());

            if (op.empty() || mode.empty())
                continue;

            result.push_back(AppOpEntry{ op, mode });
        }

        return result;
    }

    AdbCommandResult AdbPackages::setCompatFlag(std::string_view packageName, std::string_view changeId, bool enabled) const
    {
        return m_shell.runRaw(
            std::string("am compat ") + (enabled ? "enable " : "disable ") +
            std::string(quoteForShell(changeId)) + " " + std::string(quoteForShell(packageName)));
    }

    AdbCommandResult AdbPackages::clearData(std::string_view packageName) const
    {
        return m_shell.runRaw("pm clear " + quoteForShell(packageName));
    }

    AdbCommandResult AdbPackages::forceStop(std::string_view packageName) const
    {
        return m_shell.runRaw("am force-stop " + quoteForShell(packageName));
    }

    AdbCommandResult AdbPackages::uninstall(std::string_view packageName) const
    {
        return m_shell.executor()->run(m_shell.serial(), { "uninstall", std::string(packageName) });
    }

    AdbCommandResult AdbPackages::startActivity(std::string_view component, std::vector<std::string> extraArgs) const
    {
        std::string cmd = "am start -n " + quoteForShell(component);

        for (const auto& arg : extraArgs)
            cmd += " " + quoteForShell(arg);

        return m_shell.runRaw(cmd);
    }

    std::string AdbPackages::dumpPackage(std::string_view packageName) const
    {
        return m_shell.runRaw("dumpsys package " + quoteForShell(packageName)).stdoutText;
    }

    const std::shared_ptr<AdbCommandExecutor>& AdbPackages::executor() const noexcept
    {
        return m_shell.executor();
    }
}
