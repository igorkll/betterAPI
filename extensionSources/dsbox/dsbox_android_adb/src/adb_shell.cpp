#include <dsbox_android_adb/adb_shell.hpp>

#include <stdexcept>
#include <utility>

namespace dsbox_android_adb
{
    namespace
    {
        // Wraps `text` in single quotes for a POSIX shell (the shell
        // adb shell itself invokes on-device), escaping any embedded
        // single quote as '\'' -- the standard POSIX idiom (close the
        // quote, emit an escaped quote, reopen it). Keeps arbitrary
        // paths/commands with spaces, globs, etc. from being split or
        // expanded by the remote shell.
        std::string quoteForShell(const std::string& text)
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

        std::string stripTrailingNewline(std::string text)
        {
            if (!text.empty() && text.back() == '\n')
                text.pop_back();

            if (!text.empty() && text.back() == '\r')
                text.pop_back();

            return text;
        }

        std::vector<std::string> splitLines(const std::string& text)
        {
            std::vector<std::string> lines;

            std::size_t start = 0;
            while (start <= text.size())
            {
                const std::size_t newline = text.find('\n', start);
                const std::string line = (newline == std::string::npos)
                    ? text.substr(start)
                    : text.substr(start, newline - start);

                std::string trimmed = line;
                if (!trimmed.empty() && trimmed.back() == '\r')
                    trimmed.pop_back();

                if (!trimmed.empty())
                    lines.push_back(std::move(trimmed));

                if (newline == std::string::npos)
                    break;

                start = newline + 1;
            }

            return lines;
        }
    }

    AdbShell::AdbShell(std::shared_ptr<AdbCommandExecutor> executor, std::string serial)
        : m_executor(std::move(executor)), m_serial(std::move(serial))
    {
        if (!m_executor)
            throw std::invalid_argument("dsbox_android_adb: AdbShell requires a non-null AdbCommandExecutor");
    }

    const std::string& AdbShell::serial() const noexcept
    {
        return m_serial;
    }

    AdbCommandResult AdbShell::runRaw(std::string cmd) const
    {
        return m_executor->run(m_serial, { "shell", std::move(cmd) });
    }

    std::string AdbShell::run(std::string cmd) const
    {
        return stripTrailingNewline(runRaw(std::move(cmd)).stdoutText);
    }

    bool AdbShell::exists(std::string path) const
    {
        const auto result = runRaw("[ -e " + quoteForShell(path) + " ] && echo 1 || echo 0");
        return stripTrailingNewline(result.stdoutText) == "1";
    }

    std::vector<std::string> AdbShell::listDir(std::string path) const
    {
        return splitLines(runRaw("ls -1 " + quoteForShell(path)).stdoutText);
    }

    std::string AdbShell::getProp(std::string key) const
    {
        return run("getprop " + quoteForShell(key));
    }

    const std::shared_ptr<AdbCommandExecutor>& AdbShell::executor() const noexcept
    {
        return m_executor;
    }
}
