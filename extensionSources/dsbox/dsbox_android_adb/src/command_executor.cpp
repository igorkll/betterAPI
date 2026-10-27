#include <dsbox_android_adb/command_executor.hpp>

#include <dsbox_process/subprocess.hpp>
#include <dsbox_scheduler/executor.hpp>

#include <algorithm>
#include <mutex>
#include <thread>

namespace dsbox_android_adb
{
    struct AdbCommandExecutor::Impl
    {
        std::string adbPath;
        std::size_t threads = 0;

        // The pool is started lazily (see the class docs): constructing
        // an AdbCommandExecutor that's only ever used for run() from the
        // caller's own thread should not spin up worker threads it will
        // never use.
        mutable std::mutex poolMutex;
        mutable std::unique_ptr<dsbox_scheduler::ThreadPoolExecutor> pool;

        dsbox_scheduler::ThreadPoolExecutor& poolRef() const
        {
            std::lock_guard<std::mutex> lock(poolMutex);

            if (!pool)
                pool = std::make_unique<dsbox_scheduler::ThreadPoolExecutor>(threads);

            return *pool;
        }
    };

    AdbCommandExecutor::AdbCommandExecutor(std::string adbPath, std::size_t threads)
        : m_impl(std::make_unique<Impl>())
    {
        m_impl->adbPath = std::move(adbPath);
        m_impl->threads = threads;
    }

    AdbCommandExecutor::~AdbCommandExecutor() = default;

    const std::string& AdbCommandExecutor::adbPath() const noexcept
    {
        return m_impl->adbPath;
    }

    std::size_t AdbCommandExecutor::threadCount() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_impl->poolMutex);

        if (m_impl->pool)
            return m_impl->pool->threadCount();

        // Pool not started yet -- report what it WOULD be sized to,
        // mirroring ThreadPoolExecutor's own "0 means hardware_concurrency"
        // convention, so callers can size batches sensibly before the
        // first parallelForEach()/post() call.
        return m_impl->threads != 0
            ? m_impl->threads
            : std::max<std::size_t>(1, std::thread::hardware_concurrency());
    }

    AdbCommandResult AdbCommandExecutor::run(std::string_view serial, std::vector<std::string> arguments) const
    {
        dsbox_process::SubprocessOptions options;
        options.executable = m_impl->adbPath;

        std::vector<std::string> fullArgs;
        fullArgs.reserve(arguments.size() + 2);

        if (!serial.empty())
        {
            fullArgs.push_back("-s");
            fullArgs.push_back(std::string(serial));
        }

        for (auto& argument : arguments)
            fullArgs.push_back(std::move(argument));

        options.arguments = std::move(fullArgs);

        dsbox_process::Subprocess process(options);

        AdbCommandResult result;
        std::mutex outputMutex;

        process.onStdoutData([&](std::span<const std::byte> data)
        {
            std::lock_guard<std::mutex> lock(outputMutex);
            result.stdoutText.append(reinterpret_cast<const char*>(data.data()), data.size());
        });

        process.onStderrLine([&](std::string line)
        {
            std::lock_guard<std::mutex> lock(outputMutex);

            if (!result.stderrText.empty())
                result.stderrText += '\n';

            result.stderrText += line;
        });

        result.exitCode = process.wait();
        return result;
    }

    bool AdbCommandExecutor::post(std::function<void()> job) const
    {
        return m_impl->poolRef().post(std::move(job));
    }
}
