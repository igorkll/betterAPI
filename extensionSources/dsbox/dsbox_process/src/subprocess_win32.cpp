// Win32 implementation of dsbox_process::Subprocess. Compiled only on
// Windows (see CMakeLists.txt) -- see subprocess_posix.cpp for the
// POSIX counterpart. Keep the two in sync when changing behavior.
#include <dsbox_process/subprocess.hpp>

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace dsbox_process
{
    namespace
    {
        std::wstring utf8ToWide(const std::string& text)
        {
            if (text.empty())
                return std::wstring();

            const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring result(static_cast<size_t>(required), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required);
            return result;
        }

        // Quotes a single argument per the rules CommandLineToArgvW
        // expects (the same rules the MSVC CRT uses to build argv).
        void appendQuotedArg(std::wstring& commandLine, const std::wstring& arg)
        {
            if (!commandLine.empty())
                commandLine += L' ';

            const bool needsQuotes = arg.empty() || arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;

            if (!needsQuotes)
            {
                commandLine += arg;
                return;
            }

            commandLine += L'"';

            size_t backslashes = 0;
            for (const wchar_t ch : arg)
            {
                if (ch == L'\\')
                {
                    ++backslashes;
                    continue;
                }

                if (ch == L'"')
                {
                    commandLine.append(backslashes * 2 + 1, L'\\');
                    backslashes = 0;
                    commandLine += L'"';
                    continue;
                }

                commandLine.append(backslashes, L'\\');
                backslashes = 0;
                commandLine += ch;
            }

            commandLine.append(backslashes * 2, L'\\');
            commandLine += L'"';
        }

        std::wstring buildCommandLine(const SubprocessOptions& options)
        {
            std::wstring commandLine;
            appendQuotedArg(commandLine, utf8ToWide(options.executable));
            for (const std::string& arg : options.arguments)
                appendQuotedArg(commandLine, utf8ToWide(arg));
            return commandLine;
        }

        // Builds a Win32 environment block (a run of NUL-terminated
        // "KEY=VALUE" strings, terminated by an extra NUL) reflecting
        // SubprocessOptions::environment / inheritParentEnvironment.
        // Only called when the child's environment needs to differ from
        // a plain inherited copy -- see its call site.
        std::vector<wchar_t> buildEnvironmentBlock(const SubprocessOptions& options)
        {
            // Ordered map so entries from options.environment can
            // override an inherited variable of the same name; Windows
            // environment variable names are case-insensitive.
            struct CaseInsensitiveLess
            {
                bool operator()(const std::wstring& a, const std::wstring& b) const
                {
                    return _wcsicmp(a.c_str(), b.c_str()) < 0;
                }
            };

            std::map<std::wstring, std::wstring, CaseInsensitiveLess> vars;

            if (options.inheritParentEnvironment)
            {
                if (LPWCH parentEnv = GetEnvironmentStringsW())
                {
                    for (LPWCH p = parentEnv; *p; )
                    {
                        std::wstring entry(p);
                        const size_t eq = entry.find(L'=');
                        // Windows keeps a few entries starting with '='
                        // (per-drive current directory tracking, e.g.
                        // "=C:=C:\foo") -- skip anything with no name
                        // before the '='.
                        if (eq != std::wstring::npos && eq > 0)
                            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
                        p += entry.size() + 1;
                    }
                    FreeEnvironmentStringsW(parentEnv);
                }
            }

            for (const auto& [key, value] : options.environment)
                vars[utf8ToWide(key)] = utf8ToWide(value);

            std::vector<wchar_t> block;
            for (const auto& [key, value] : vars)
            {
                std::wstring entry = key + L'=' + value;
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(L'\0');
            }
            block.push_back(L'\0'); // extra NUL terminating the whole block

            return block;
        }

        struct Pipe
        {
            HANDLE readEnd = nullptr;
            HANDLE writeEnd = nullptr;
        };

        // Creates a pipe and prevents the end we keep for ourselves from
        // being inherited by the child (only the end the child should
        // use is left inheritable) -- the standard CreatePipe dance.
        Pipe createPipe(bool parentKeepsRead)
        {
            SECURITY_ATTRIBUTES securityAttributes{};
            securityAttributes.nLength = sizeof(securityAttributes);
            securityAttributes.bInheritHandle = TRUE;

            Pipe pipe;
            if (!CreatePipe(&pipe.readEnd, &pipe.writeEnd, &securityAttributes, 0))
                throw std::runtime_error("dsbox_process: CreatePipe failed");

            HANDLE& ourEnd = parentKeepsRead ? pipe.readEnd : pipe.writeEnd;
            if (!SetHandleInformation(ourEnd, HANDLE_FLAG_INHERIT, 0))
                throw std::runtime_error("dsbox_process: SetHandleInformation failed");

            return pipe;
        }
    }

    struct Subprocess::Impl
    {
        PROCESS_INFORMATION processInfo{};
        HANDLE stdinWrite = nullptr;
        HANDLE stdoutRead = nullptr;
        HANDLE stderrRead = nullptr;

        std::function<void(std::span<const std::byte>)> onStdoutData;
        std::function<void(std::string)> onStderrLine;
        std::function<void(int)> onExitCb;

        std::thread stdoutThread;
        std::thread stderrThread;
        std::thread stdinThread;
        std::thread waiterThread;

        std::mutex stdinMutex;
        std::condition_variable stdinCv;
        std::deque<std::byte> stdinQueue;
        bool stdinCloseRequested = false;
        bool stdinThreadStop = false;

        std::mutex exitMutex;
        std::condition_variable exitCv;
        bool exited = false;
        int exitCode = 0;

        std::atomic<bool> running{ true };

        ~Impl()
        {
            // Destroying a live child would otherwise leave reader
            // threads blocked on ReadFile forever -- force it down so
            // this object never hangs the caller's shutdown.
            if (running.load())
                TerminateProcess(processInfo.hProcess, 1);

            {
                std::lock_guard<std::mutex> lock(stdinMutex);
                stdinThreadStop = true;
            }
            stdinCv.notify_all();

            if (stdinThread.joinable())
                stdinThread.join();
            if (stdoutThread.joinable())
                stdoutThread.join();
            if (stderrThread.joinable())
                stderrThread.join();
            if (waiterThread.joinable())
                waiterThread.join();

            if (stdinWrite)
                CloseHandle(stdinWrite);
            if (stdoutRead)
                CloseHandle(stdoutRead);
            if (stderrRead)
                CloseHandle(stderrRead);
            if (processInfo.hProcess)
                CloseHandle(processInfo.hProcess);
            if (processInfo.hThread)
                CloseHandle(processInfo.hThread);
        }

        void runStdoutReader()
        {
            std::vector<std::byte> buffer(64 * 1024);
            DWORD bytesRead = 0;

            while (ReadFile(stdoutRead, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
            {
                if (onStdoutData)
                    onStdoutData(std::span<const std::byte>(buffer.data(), bytesRead));
            }
        }

        void runStderrReader()
        {
            std::string lineBuffer;
            char buffer[64 * 1024];
            DWORD bytesRead = 0;

            while (ReadFile(stderrRead, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
            {
                lineBuffer.append(buffer, bytesRead);

                size_t newlinePos;
                while ((newlinePos = lineBuffer.find('\n')) != std::string::npos)
                {
                    std::string line = lineBuffer.substr(0, newlinePos);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();

                    if (onStderrLine)
                        onStderrLine(std::move(line));

                    lineBuffer.erase(0, newlinePos + 1);
                }
            }

            if (!lineBuffer.empty() && onStderrLine)
                onStderrLine(std::move(lineBuffer));
        }

        void runStdinWriter()
        {
            for (;;)
            {
                std::vector<std::byte> chunk;
                bool shouldClose = false;

                {
                    std::unique_lock<std::mutex> lock(stdinMutex);
                    stdinCv.wait(lock, [this]
                    {
                        return !stdinQueue.empty() || stdinCloseRequested || stdinThreadStop;
                    });

                    if (stdinThreadStop)
                        return;

                    if (!stdinQueue.empty())
                    {
                        chunk.assign(stdinQueue.begin(), stdinQueue.end());
                        stdinQueue.clear();
                    }
                    else if (stdinCloseRequested)
                    {
                        shouldClose = true;
                    }
                }

                if (!chunk.empty())
                {
                    DWORD written = 0;
                    if (!WriteFile(stdinWrite, chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr))
                        return; // child gone / pipe broken -- nothing more to do
                }

                if (shouldClose)
                {
                    CloseHandle(stdinWrite);
                    stdinWrite = nullptr;
                    return;
                }
            }
        }

        void runWaiter()
        {
            WaitForSingleObject(processInfo.hProcess, INFINITE);

            DWORD code = 0;
            GetExitCodeProcess(processInfo.hProcess, &code);

            running.store(false);

            {
                std::lock_guard<std::mutex> lock(exitMutex);
                exited = true;
                exitCode = static_cast<int>(code);
            }
            exitCv.notify_all();

            if (onExitCb)
                onExitCb(static_cast<int>(code));
        }
    };

    Subprocess::Subprocess(SubprocessOptions options)
        : m_impl(std::make_unique<Impl>())
    {
        Pipe stdinPipe = createPipe(/*parentKeepsRead=*/false);
        Pipe stdoutPipe = createPipe(/*parentKeepsRead=*/true);
        Pipe stderrPipe = createPipe(/*parentKeepsRead=*/true);

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdInput = stdinPipe.readEnd;
        startupInfo.hStdOutput = stdoutPipe.writeEnd;
        startupInfo.hStdError = stderrPipe.writeEnd;

        std::wstring commandLine = buildCommandLine(options);
        std::wstring workingDirectory = options.workingDirectory ? utf8ToWide(*options.workingDirectory) : std::wstring();

        // Only build a custom environment block when the child's
        // environment needs to differ from a plain inherited copy --
        // lpEnvironment == nullptr already gives the default (and
        // previous) behavior of inheriting everything as-is.
        std::vector<wchar_t> environmentBlock;
        LPVOID environmentPtr = nullptr;
        DWORD creationFlags = CREATE_NO_WINDOW;

        if (!options.inheritParentEnvironment || !options.environment.empty())
        {
            environmentBlock = buildEnvironmentBlock(options);
            environmentPtr = environmentBlock.data();
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
        }

        const BOOL ok = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            /*bInheritHandles=*/TRUE,
            creationFlags,
            environmentPtr,
            options.workingDirectory ? workingDirectory.c_str() : nullptr,
            &startupInfo,
            &m_impl->processInfo);

        // The child's ends of the pipes must be closed in the parent
        // regardless of success/failure, or ReadFile on our end will
        // never see EOF once the child exits (it would still see the
        // handle as open via our own leaked copy).
        CloseHandle(stdinPipe.readEnd);
        CloseHandle(stdoutPipe.writeEnd);
        CloseHandle(stderrPipe.writeEnd);

        if (!ok)
        {
            CloseHandle(stdinPipe.writeEnd);
            CloseHandle(stdoutPipe.readEnd);
            CloseHandle(stderrPipe.readEnd);
            throw std::runtime_error("dsbox_process: CreateProcessW failed for '" + options.executable + "'");
        }

        m_impl->stdinWrite = stdinPipe.writeEnd;
        m_impl->stdoutRead = stdoutPipe.readEnd;
        m_impl->stderrRead = stderrPipe.readEnd;

        m_impl->stdoutThread = std::thread([impl = m_impl.get()] { impl->runStdoutReader(); });
        m_impl->stderrThread = std::thread([impl = m_impl.get()] { impl->runStderrReader(); });
        m_impl->stdinThread = std::thread([impl = m_impl.get()] { impl->runStdinWriter(); });
        m_impl->waiterThread = std::thread([impl = m_impl.get()] { impl->runWaiter(); });
    }

    Subprocess::~Subprocess() = default;

    void Subprocess::onStdoutData(std::function<void(std::span<const std::byte>)> callback)
    {
        m_impl->onStdoutData = std::move(callback);
    }

    void Subprocess::onStderrLine(std::function<void(std::string)> callback)
    {
        m_impl->onStderrLine = std::move(callback);
    }

    void Subprocess::onExit(std::function<void(int)> callback)
    {
        m_impl->onExitCb = std::move(callback);
    }

    void Subprocess::writeStdin(std::span<const std::byte> data)
    {
        {
            std::lock_guard<std::mutex> lock(m_impl->stdinMutex);
            m_impl->stdinQueue.insert(m_impl->stdinQueue.end(), data.begin(), data.end());
        }
        m_impl->stdinCv.notify_one();
    }

    void Subprocess::closeStdin()
    {
        {
            std::lock_guard<std::mutex> lock(m_impl->stdinMutex);
            m_impl->stdinCloseRequested = true;
        }
        m_impl->stdinCv.notify_one();
    }

    bool Subprocess::isRunning() const
    {
        return m_impl->running.load();
    }

    int Subprocess::wait()
    {
        std::unique_lock<std::mutex> lock(m_impl->exitMutex);
        m_impl->exitCv.wait(lock, [this] { return m_impl->exited; });
        return m_impl->exitCode;
    }
}
