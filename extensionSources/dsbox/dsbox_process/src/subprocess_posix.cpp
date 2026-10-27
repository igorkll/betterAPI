// POSIX implementation of dsbox_process::Subprocess. Compiled only on
// non-Windows platforms (see CMakeLists.txt) -- see subprocess_win32.cpp
// for the Windows counterpart. Keep the two in sync when changing
// behavior.
#include <dsbox_process/subprocess.hpp>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <csignal>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

extern char** environ;

namespace dsbox_process
{
    namespace
    {
        struct Pipe
        {
            int readEnd = -1;
            int writeEnd = -1;
        };

        Pipe makePipe()
        {
            int fds[2];
            if (pipe(fds) != 0)
                throw std::runtime_error("dsbox_process: pipe() failed");
            return Pipe{ fds[0], fds[1] };
        }

        ssize_t readRetryEintr(int fd, void* buffer, size_t count)
        {
            for (;;)
            {
                const ssize_t result = ::read(fd, buffer, count);
                if (result >= 0 || errno != EINTR)
                    return result;
            }
        }

        ssize_t writeRetryEintr(int fd, const void* buffer, size_t count)
        {
            for (;;)
            {
                const ssize_t result = ::write(fd, buffer, count);
                if (result >= 0 || errno != EINTR)
                    return result;
            }
        }

        // Builds a POSIX envp-style list of "KEY=VALUE" strings
        // reflecting SubprocessOptions::environment /
        // inheritParentEnvironment. Only called when the child's
        // environment needs to differ from a plain inherited copy --
        // see its call site.
        std::vector<std::string> buildEnvironmentStrings(const SubprocessOptions& options)
        {
            // Ordered map so entries from options.environment can
            // override an inherited variable of the same name.
            std::map<std::string, std::string> vars;

            if (options.inheritParentEnvironment)
            {
                for (char** entryPtr = environ; *entryPtr; ++entryPtr)
                {
                    std::string entry(*entryPtr);
                    const size_t eq = entry.find('=');
                    if (eq != std::string::npos)
                        vars[entry.substr(0, eq)] = entry.substr(eq + 1);
                }
            }

            for (const auto& [key, value] : options.environment)
                vars[key] = value;

            std::vector<std::string> result;
            result.reserve(vars.size());
            for (const auto& [key, value] : vars)
                result.push_back(key + "=" + value);

            return result;
        }
    }

    struct Subprocess::Impl
    {
        pid_t pid = -1;
        int stdinWrite = -1;
        int stdoutRead = -1;
        int stderrRead = -1;

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
            // Mirrors subprocess_win32.cpp: never let a still-running
            // child hang this destructor forever.
            if (running.load() && pid > 0)
                kill(pid, SIGKILL);

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

            if (stdinWrite >= 0)
                close(stdinWrite);
            if (stdoutRead >= 0)
                close(stdoutRead);
            if (stderrRead >= 0)
                close(stderrRead);
        }

        void runStdoutReader()
        {
            std::vector<std::byte> buffer(64 * 1024);

            for (;;)
            {
                const ssize_t bytesRead = readRetryEintr(stdoutRead, buffer.data(), buffer.size());
                if (bytesRead <= 0)
                    return;

                if (onStdoutData)
                    onStdoutData(std::span<const std::byte>(buffer.data(), static_cast<size_t>(bytesRead)));
            }
        }

        void runStderrReader()
        {
            std::string lineBuffer;
            char buffer[64 * 1024];

            for (;;)
            {
                const ssize_t bytesRead = readRetryEintr(stderrRead, buffer, sizeof(buffer));
                if (bytesRead <= 0)
                    break;

                lineBuffer.append(buffer, static_cast<size_t>(bytesRead));

                size_t newlinePos;
                while ((newlinePos = lineBuffer.find('\n')) != std::string::npos)
                {
                    std::string line = lineBuffer.substr(0, newlinePos);
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
                    size_t offset = 0;
                    while (offset < chunk.size())
                    {
                        const ssize_t written = writeRetryEintr(stdinWrite, chunk.data() + offset, chunk.size() - offset);
                        if (written <= 0)
                            return; // child gone / broken pipe -- nothing more to do
                        offset += static_cast<size_t>(written);
                    }
                }

                if (shouldClose)
                {
                    close(stdinWrite);
                    stdinWrite = -1;
                    return;
                }
            }
        }

        void runWaiter()
        {
            int status = 0;
            waitpid(pid, &status, 0);

            const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            running.store(false);

            {
                std::lock_guard<std::mutex> lock(exitMutex);
                exited = true;
                exitCode = code;
            }
            exitCv.notify_all();

            if (onExitCb)
                onExitCb(code);
        }
    };

    Subprocess::Subprocess(SubprocessOptions options)
        : m_impl(std::make_unique<Impl>())
    {
        Pipe stdinPipe = makePipe();
        Pipe stdoutPipe = makePipe();
        Pipe stderrPipe = makePipe();

        std::vector<std::string> argStorage;
        argStorage.push_back(options.executable);
        for (const std::string& arg : options.arguments)
            argStorage.push_back(arg);

        std::vector<char*> argv;
        argv.reserve(argStorage.size() + 1);
        for (std::string& arg : argStorage)
            argv.push_back(arg.data());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t fileActions;
        posix_spawn_file_actions_init(&fileActions);

        posix_spawn_file_actions_adddup2(&fileActions, stdinPipe.readEnd, STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe.writeEnd, STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fileActions, stderrPipe.writeEnd, STDERR_FILENO);

        // Close every pipe fd in the child once the dup2s above have
        // pointed 0/1/2 at them -- the originals are redundant there and
        // would otherwise leak into the child, keeping our own read/write
        // ends artificially alive.
        posix_spawn_file_actions_addclose(&fileActions, stdinPipe.readEnd);
        posix_spawn_file_actions_addclose(&fileActions, stdinPipe.writeEnd);
        posix_spawn_file_actions_addclose(&fileActions, stdoutPipe.readEnd);
        posix_spawn_file_actions_addclose(&fileActions, stdoutPipe.writeEnd);
        posix_spawn_file_actions_addclose(&fileActions, stderrPipe.readEnd);
        posix_spawn_file_actions_addclose(&fileActions, stderrPipe.writeEnd);

        if (options.workingDirectory)
            posix_spawn_file_actions_addchdir_np(&fileActions, options.workingDirectory->c_str());

        // Only build a custom envp when the child's environment needs to
        // differ from a plain inherited copy -- passing the process's
        // own `environ` straight through (as before) already gives the
        // default/previous behavior of inheriting everything as-is.
        std::vector<std::string> envStorage;
        std::vector<char*> envp;
        char** envpPtr = environ;

        if (!options.inheritParentEnvironment || !options.environment.empty())
        {
            envStorage = buildEnvironmentStrings(options);
            envp.reserve(envStorage.size() + 1);
            for (std::string& entry : envStorage)
                envp.push_back(entry.data());
            envp.push_back(nullptr);
            envpPtr = envp.data();
        }

        // posix_spawnp (not posix_spawn) so a bare name like "sort" is
        // resolved via PATH -- matching CreateProcessW's behavior on the
        // Win32 side when lpApplicationName is null.
        pid_t pid = -1;
        const int spawnResult = posix_spawnp(&pid, options.executable.c_str(), &fileActions, nullptr, argv.data(), envpPtr);

        posix_spawn_file_actions_destroy(&fileActions);

        // Parent no longer needs the child's ends, regardless of outcome.
        close(stdinPipe.readEnd);
        close(stdoutPipe.writeEnd);
        close(stderrPipe.writeEnd);

        if (spawnResult != 0)
        {
            close(stdinPipe.writeEnd);
            close(stdoutPipe.readEnd);
            close(stderrPipe.readEnd);
            throw std::runtime_error("dsbox_process: posix_spawn failed for '" + options.executable + "': " + std::strerror(spawnResult));
        }

        m_impl->pid = pid;
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
