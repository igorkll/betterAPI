#pragma once

#include <dsbox_android_adb/api.hpp>

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dsbox_android_adb
{
    // Result of a single `adb` invocation run to completion. A nonzero
    // exitCode is NOT turned into an exception anywhere in this library
    // -- adb itself uses the exit code to report ordinary, expected
    // failures ("device not found", "package not found", a failing
    // shell command), so every caller gets the full result and decides
    // for itself what counts as success.
    struct AdbCommandResult
    {
        int exitCode = 0;
        std::string stdoutText;
        std::string stderrText;

        bool ok() const noexcept
        {
            return exitCode == 0;
        }
    };

    // The shared backend behind every live handle in this library --
    // AdbDevice, and the AdbShell/AdbPackages wrappers it hands out.
    // Holds the `adb` executable path plus a lazily-started
    // dsbox_scheduler::ThreadPoolExecutor (kept behind PIMPL so this
    // header doesn't have to include dsbox_scheduler/executor.hpp) that
    // parallel helpers (parallelForEach() below, AdbModule::forEachDevice())
    // use to run several `adb` subprocesses concurrently instead of one
    // after another.
    //
    // A single AdbDevice::shell() call does not need the pool itself --
    // spawning one subprocess and blocking on it is already as parallel
    // as a lone call can be. The pool only pays off once several
    // commands are issued at once, which is the whole point of handing
    // this same executor to every handle: ten AdbDevice objects sharing
    // one AdbCommandExecutor, each driven from a lambda passed to
    // parallelForEach(), spawn up to `threads` adb processes at a time
    // instead of queuing behind each other.
    //
    // Reference-counted and independent of AdbModule's own lifetime: a
    // handle obtained from AdbModule::device() keeps working (and keeps
    // its slice of the pool) even if the AdbModule that created it is
    // later destroyed -- construct one directly (no Context/IModule
    // required at all) to use AdbDevice/AdbShell/AdbPackages completely
    // standalone, e.g. in a unit test or a tool with no module system.
    //
    // Thread-safe: every member function may be called concurrently from
    // any thread, including from within a job running on this executor's
    // own pool.
    class DSBOX_ANDROID_ADB_API AdbCommandExecutor : public std::enable_shared_from_this<AdbCommandExecutor>
    {
    public:
        // `adbPath` is passed as SubprocessOptions::executable for every
        // invocation -- a bare name ("adb", the default) is resolved via
        // PATH, a full path is used as-is. `threads` is the pool's
        // worker count; 0 (the default) means
        // std::thread::hardware_concurrency() -- see
        // dsbox_scheduler::ThreadPoolExecutor. The pool itself is
        // started lazily on first use, so constructing an
        // AdbCommandExecutor that only ever calls run()/shell() from the
        // caller's own thread costs nothing beyond the object itself.
        explicit AdbCommandExecutor(std::string adbPath = "adb", std::size_t threads = 0);
        ~AdbCommandExecutor();

        AdbCommandExecutor(const AdbCommandExecutor&) = delete;
        AdbCommandExecutor& operator=(const AdbCommandExecutor&) = delete;

        const std::string& adbPath() const noexcept;

        std::size_t threadCount() const noexcept;

        // Runs `adb [-s serial] arguments...` to completion on the
        // CALLING thread and returns its result. `serial` empty omits
        // "-s <serial>" entirely -- for adb's own global commands
        // ("devices", "start-server", "connect <host:port>", ...).
        // Throws std::runtime_error if adb could not be spawned at all
        // (not found, not executable -- propagated from
        // dsbox_process::Subprocess's constructor); a nonzero exit code
        // from adb itself is reported via AdbCommandResult::exitCode,
        // never as an exception.
        AdbCommandResult run(std::string_view serial, std::vector<std::string> arguments) const;

        // Queues `job` on this executor's pool and returns immediately.
        // The low-level primitive parallelForEach() below is built on --
        // exposed directly for callers that want to build their own
        // batch helpers without going through it. Returns false if the
        // job was not accepted (the pool has been shut down, which only
        // happens as this object is being destroyed -- not a case a
        // normal caller needs to handle).
        bool post(std::function<void()> job) const;

        // Runs `work(items[i])` once per element of `items`, in
        // parallel on this executor's pool, and blocks the calling
        // thread until every one has finished. Results come back in a
        // std::vector in the SAME order as `items` (not completion
        // order). If one or more invocations of `work` throw, the first
        // exception (by index, not by completion order) is rethrown
        // here after every invocation has finished running -- a failing
        // device never stops the others from being driven, it only
        // determines what parallelForEach() itself reports.
        //
        // This is the piece that answers "how do I drive N devices at
        // once": e.g.
        //
        //   auto results = executor->parallelForEach(serials,
        //       [&](const std::string& serial) {
        //           return module.device(serial).shell("getprop ro.build.version.release");
        //       });
        //
        // runs that shell command against every serial concurrently
        // (bounded by threadCount() workers) instead of one after
        // another.
        template<typename T, typename Work>
        auto parallelForEach(const std::vector<T>& items, Work work) const
            -> std::vector<std::invoke_result_t<Work, const T&>>
        {
            using Result = std::invoke_result_t<Work, const T&>;
            static_assert(!std::is_void_v<Result>,
                "parallelForEach: Work must return a value (std::vector<void> isn't a thing) -- "
                "wrap a void-returning call in a lambda that returns e.g. AdbCommandResult or bool");

            std::vector<std::packaged_task<Result()>> tasks;
            std::vector<std::future<Result>> futures;
            tasks.reserve(items.size());
            futures.reserve(items.size());

            for (const auto& item : items)
            {
                std::packaged_task<Result()> task([&work, &item] { return work(item); });
                futures.push_back(task.get_future());
                tasks.push_back(std::move(task));
            }

            for (auto& task : tasks)
            {
                // Each packaged_task is posted by reference to this
                // executor's own storage (not moved into the lambda) so
                // the vector above keeps the task object alive; the pool
                // only ever runs it once.
                std::packaged_task<Result()>* taskPtr = &task;
                if (!post([taskPtr] { (*taskPtr)(); }))
                    (*taskPtr)(); // pool rejected the job (shutting down) -- run it inline rather than deadlock on its future
            }

            std::vector<Result> results;
            results.reserve(futures.size());

            std::exception_ptr firstError;
            for (auto& future : futures)
            {
                try
                {
                    results.push_back(future.get());
                }
                catch (...)
                {
                    if (!firstError)
                        firstError = std::current_exception();

                    results.emplace_back(); // keep index alignment with `items`
                }
            }

            if (firstError)
                std::rethrow_exception(firstError);

            return results;
        }

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
