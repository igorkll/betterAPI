#pragma once

#include <dsbox_scheduler/api.hpp>
#include <dsbox_scheduler/types.hpp>

#include <cstddef>
#include <memory>

namespace dsbox_scheduler
{
    // Where a Callback runs. This is the seam that lets timers and
    // coroutines resume on "the right thread" without the scheduler
    // knowing what that thread is: a thread pool, a UI/event loop pumped
    // by the host, or a module's own worker thread (implement post() on
    // top of that thread's queue and pass it in).
    class IExecutor
    {
    public:
        virtual ~IExecutor() = default;

        // Queues `callback` to run later, on whatever thread(s) this
        // executor owns. Thread-safe.
        //
        // Returns false if the executor is shut down/closed and did NOT
        // take the callback -- it will never run, and the caller decides
        // what to do (the scheduler treats it as "cancelled").
        //
        // A callback must not be assumed to run before post() returns,
        // nor after (except for InlineExecutor, which runs it before).
        virtual bool post(Callback callback) = 0;
    };

    // Wraps a caller-owned executor (e.g. a LoopExecutor living on the
    // stack) in the shared_ptr the scheduler API wants, WITHOUT taking
    // ownership. The executor must outlive everything that was handed
    // the returned pointer -- including timers still pending on it.
    inline std::shared_ptr<IExecutor> nonOwning(IExecutor& executor) noexcept
    {
        return std::shared_ptr<IExecutor>(std::shared_ptr<void>(), &executor);
    }

    namespace detail
    {
        // Runs `callback`, routing any exception to `onError` (or
        // swallowing it if there is none).
        inline void invokeGuarded(const Callback& callback, const ErrorHandler& onError) noexcept
        {
            try
            {
                if (callback)
                    callback();
            }
            catch (...)
            {
                if (onError)
                {
                    try
                    {
                        onError(std::current_exception());
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        struct PoolState;
        struct LoopState;
        struct SerialState;
    }

    // Runs the callback immediately, on the posting thread, before
    // post() returns. An exception thrown by the callback propagates to
    // the caller of post(). Mostly for tests and for "no thread hop
    // wanted" call sites.
    class InlineExecutor final : public IExecutor
    {
    public:
        bool post(Callback callback) override
        {
            if (callback)
                callback();

            return true;
        }
    };

    // A pool of worker threads draining one FIFO queue.
    //
    //  - Threads are started lazily, on the first post(): an unused pool
    //    costs nothing.
    //  - An exception escaping a callback goes to the ErrorHandler (or is
    //    swallowed); the worker survives.
    //  - shutdown() (also run by the destructor) stops accepting new
    //    callbacks, lets the ones already queued finish, then joins.
    //    Calling it from inside one of the pool's own callbacks is
    //    allowed: that worker is detached instead of joined.
    class DSBOX_SCHEDULER_API ThreadPoolExecutor final : public IExecutor
    {
    public:
        // threads == 0 means std::thread::hardware_concurrency() (at
        // least 1).
        explicit ThreadPoolExecutor(std::size_t threads = 0, ErrorHandler onError = {});
        ~ThreadPoolExecutor() override;

        ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
        ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

        bool post(Callback callback) override;

        void shutdown();

        // Blocks until the queue is empty and no callback is running.
        // Callbacks posted by other threads meanwhile extend the wait.
        void waitIdle();

        std::size_t threadCount() const noexcept;

    private:
        std::shared_ptr<detail::PoolState> m_state;
    };

    // A queue drained by threads the HOST provides, by calling run(),
    // runFor(), runOne() or poll() -- typically its main/UI thread. This
    // is how a single-threaded event loop becomes a target for timers
    // and coroutines: pass the loop as the executor and everything
    // resumes on the thread that pumps it.
    //
    // All methods are thread-safe; several threads may pump at once.
    class DSBOX_SCHEDULER_API LoopExecutor final : public IExecutor
    {
    public:
        explicit LoopExecutor(ErrorHandler onError = {});
        ~LoopExecutor() override;

        LoopExecutor(const LoopExecutor&) = delete;
        LoopExecutor& operator=(const LoopExecutor&) = delete;

        // False once close() has been called.
        bool post(Callback callback) override;

        // Non-blocking: runs one queued callback if there is one.
        bool runOne();

        // Non-blocking: runs callbacks until the queue is empty
        // (including ones queued by the callbacks themselves). Returns
        // how many ran.
        std::size_t poll();

        // Blocks, running callbacks as they arrive, until `timeout`
        // elapses or stop() is called. Returns how many ran.
        std::size_t runFor(Duration timeout);

        // Blocks, running callbacks as they arrive, until stop() is
        // called. Returns how many ran.
        std::size_t run();

        // Makes run()/runFor() return (also if called before them: the
        // flag stays set until restart()). poll()/runOne() ignore it.
        // The queue is left intact and post() keeps working.
        void stop();
        void restart();

        // Makes post() return false from now on. Already queued
        // callbacks stay queued and can still be pumped.
        void close();

        std::size_t pending() const;

    private:
        std::shared_ptr<detail::LoopState> m_state;
    };

    // A "strand": callbacks posted to it run one at a time, in posting
    // order, on `base` -- never concurrently, even if `base` is a
    // multi-threaded pool. Lets thread-affine or non-thread-safe state be
    // protected without a mutex.
    //
    // Does not pin a thread: consecutive callbacks may run on different
    // threads of `base` (with a happens-before edge between them).
    //
    // If `base` rejects the drain task (it was shut down), the callbacks
    // queued at that moment are dropped and post() returns false.
    class DSBOX_SCHEDULER_API SerialExecutor final : public IExecutor
    {
    public:
        explicit SerialExecutor(std::shared_ptr<IExecutor> base, ErrorHandler onError = {});
        ~SerialExecutor() override;

        SerialExecutor(const SerialExecutor&) = delete;
        SerialExecutor& operator=(const SerialExecutor&) = delete;

        bool post(Callback callback) override;

    private:
        std::shared_ptr<detail::SerialState> m_state;
    };
}
