#pragma once

#include <dsbox_scheduler/api.hpp>
#include <dsbox_scheduler/clock.hpp>
#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/types.hpp>

#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace dsbox_scheduler
{
    // How a repeating timer (Scheduler::every) picks its next run.
    enum class PeriodicPolicy
    {
        // Runs at first + k*period. The next run is scheduled when the
        // current one STARTS, so a slow callback does not shift the
        // schedule. Ticks that were missed (the callback or the executor
        // was too slow, the process was suspended) are skipped, not
        // replayed in a burst. Runs may overlap if the executor is
        // multi-threaded and a callback outlasts the period -- put the
        // timer on a SerialExecutor to rule that out.
        FixedRate,

        // The next run is scheduled `period` after the current one
        // FINISHES. Runs never overlap.
        FixedDelay,
    };

    namespace detail
    {
        struct SchedulerImpl;
        struct TimerState;
        struct SleepState;
    }

    // Reference to a scheduled timer. Cheap to copy; all copies refer to
    // the same timer. Does NOT cancel the timer on destruction (see
    // ScopedTimer for that): dropping the handle of a fire-and-forget
    // timer is the normal case.
    //
    // A default-constructed handle -- which is also what a Scheduler
    // returns once it has been stopped -- is "empty": it converts to
    // false, and cancel() does nothing.
    //
    // Do not capture a periodic timer's own TimerHandle (or a shared_ptr
    // to it) inside that timer's callback: the timer owns the callback, so
    // that is a reference cycle and the timer would never be freed. Use a
    // weak_ptr, or a reference to something the caller keeps alive.
    // (A one-shot timer releases its callback after running, so it is
    // not affected.)
    class DSBOX_SCHEDULER_API TimerHandle
    {
    public:
        TimerHandle() noexcept = default;

        // True if this refers to a timer (even one that already fired or
        // was cancelled).
        explicit operator bool() const noexcept
        {
            return m_state != nullptr;
        }

        // True while the timer can still run: not yet fired (one-shot) /
        // not cancelled (periodic), and the scheduler is still running.
        bool active() const noexcept;

        // Prevents further runs, including a run already handed to an
        // executor but not started yet. Returns true if this call is the
        // one that cancelled it, false if it had already run (one-shot),
        // was already cancelled, or the handle is empty. For a one-shot
        // timer, exactly one of "it ran" and "cancel() returned true"
        // happens.
        //
        // Does NOT wait for a run that is already in progress.
        bool cancel();

        // cancel(), then blocks until no run of this timer is still
        // executing -- after it returns, the callback is guaranteed not to
        // be touching (and will never touch) objects the caller is about
        // to destroy. Safe to call from inside the timer's own callback
        // (it then just cancels, since it cannot wait for itself).
        // Deadlocks if called from something the running callback is
        // itself waiting for.
        void cancelAndWait();

    private:
        friend struct detail::SchedulerImpl;

        explicit TimerHandle(std::shared_ptr<detail::TimerState> state) noexcept
            : m_state(std::move(state))
        {
        }

        std::shared_ptr<detail::TimerState> m_state;
    };

    // Owns a timer: cancels it when destroyed, reset, or overwritten.
    // The RAII counterpart to TimerHandle, same spirit as
    // dsbox::ScopedSubscription.
    class ScopedTimer
    {
    public:
        ScopedTimer() noexcept = default;

        ScopedTimer(TimerHandle handle) noexcept // NOLINT: implicit on purpose
            : m_handle(std::move(handle))
        {
        }

        ScopedTimer(ScopedTimer&& other) noexcept
            : m_handle(std::exchange(other.m_handle, TimerHandle{}))
        {
        }

        ScopedTimer& operator=(ScopedTimer&& other) noexcept
        {
            if (this != &other)
            {
                m_handle.cancel();
                m_handle = std::exchange(other.m_handle, TimerHandle{});
            }

            return *this;
        }

        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;

        ~ScopedTimer()
        {
            m_handle.cancel();
        }

        void reset(TimerHandle handle = {}) noexcept
        {
            m_handle.cancel();
            m_handle = std::move(handle);
        }

        // Gives up ownership without cancelling.
        TimerHandle release() noexcept
        {
            return std::exchange(m_handle, TimerHandle{});
        }

        const TimerHandle& handle() const noexcept
        {
            return m_handle;
        }

        bool active() const noexcept
        {
            return m_handle.active();
        }

    private:
        TimerHandle m_handle;
    };

    struct SchedulerOptions
    {
        // Time source. null = SteadyClock.
        std::shared_ptr<IClock> clock;

        // Where timer callbacks (and coroutines resuming from
        // sleepFor) run when the call does not name an executor. null =
        // an internal ThreadPoolExecutor, created on first use. When you
        // pass your own, the scheduler never shuts it down.
        std::shared_ptr<IExecutor> defaultExecutor;

        // Size of the internal pool (0 = hardware_concurrency). Ignored
        // when defaultExecutor is set.
        std::size_t threads = 0;

        // true: a background thread, started with the first timer, waits
        // for deadlines in real time and dispatches due timers.
        // false: no thread; the host calls Scheduler::runDue() (see
        // ManualClock for the typical use).
        bool useTimerThread = true;

        // Receives exceptions escaping timer callbacks and the internal
        // pool's callbacks. null = swallow.
        ErrorHandler errorHandler;
    };

    class Scheduler;

    // Returned by Scheduler::sleepFor()/sleepUntil(); use with co_await:
    //
    //   bool elapsed = co_await scheduler.sleepFor(100ms);
    //
    // Yields true if the time elapsed, false if the sleep was cut short:
    // the stop_token fired, or the scheduler was (or already had been)
    // stopped. The coroutine resumes on the sleep's executor (default:
    // the scheduler's default executor) -- or, for a scheduler that was
    // already stopped, inline.
    //
    // The Scheduler must outlive the awaiting coroutine's suspension;
    // Scheduler::stop() (and its destructor) resume every sleeper.
    class DSBOX_SCHEDULER_API SleepAwaiter
    {
    public:
        SleepAwaiter(
            Scheduler& scheduler,
            TimePoint deadline,
            std::stop_token token,
            std::shared_ptr<IExecutor> executor);

        bool await_ready() const noexcept
        {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle);
        bool await_resume();

    private:
        Scheduler* m_scheduler;
        TimePoint m_deadline;
        std::stop_token m_token;
        std::shared_ptr<IExecutor> m_executor;
        std::shared_ptr<detail::SleepState> m_state;
    };

    // Timers, deferred operations and (with coroutines.hpp) C++20
    // coroutine support.
    //
    // Independent of the module system: create as many instances as you
    // like, anywhere, with or without a Context. SchedulerModule merely
    // publishes one shared instance for modules that want it; nothing
    // requires it.
    //
    // Nothing runs on the caller's thread and nothing is started until
    // needed: the timer thread appears with the first timer, the default
    // pool with the first callback that needs it.
    //
    // Every method is thread-safe. Callbacks may call back into the
    // scheduler (schedule, cancel, stop) freely.
    //
    // Executors: every scheduling call takes an optional executor; null
    // means the default executor. Timers scheduled for the same instant
    // are dispatched in the order they were scheduled.
    //
    // Shutdown: stop() (also run by the destructor) discards pending
    // timers, resumes every coroutine sleeping on this scheduler with
    // "cancelled", shuts down the internal pool (letting callbacks it
    // already holds finish) and joins the timer thread. Afterwards every
    // scheduling call returns an empty handle / false and nothing new
    // runs. Idempotent.
    class DSBOX_SCHEDULER_API Scheduler
    {
    public:
        explicit Scheduler(SchedulerOptions options = {});
        ~Scheduler();

        Scheduler(const Scheduler&) = delete;
        Scheduler& operator=(const Scheduler&) = delete;

        // -- timers -------------------------------------------------

        // Runs `task` once, at `when` (immediately if already past).
        TimerHandle at(TimePoint when, Callback task, std::shared_ptr<IExecutor> executor = {});

        // Runs `task` once, `delay` from now.
        TimerHandle after(Duration delay, Callback task, std::shared_ptr<IExecutor> executor = {});

        // Runs `task` every `period`, first run one period from now.
        // period must be > 0 (std::invalid_argument otherwise).
        TimerHandle every(
            Duration period,
            Callback task,
            PeriodicPolicy policy = PeriodicPolicy::FixedRate,
            std::shared_ptr<IExecutor> executor = {});

        // Like every(), first run at `first`.
        TimerHandle everyAt(
            TimePoint first,
            Duration period,
            Callback task,
            PeriodicPolicy policy = PeriodicPolicy::FixedRate,
            std::shared_ptr<IExecutor> executor = {});

        // -- deferred operations ------------------------------------

        // Runs `task` as soon as possible on `executor`. False if the
        // scheduler is stopped or the executor rejected it (then `task`
        // never runs).
        bool post(Callback task, std::shared_ptr<IExecutor> executor = {});

        // -- coroutines ---------------------------------------------

        SleepAwaiter sleepFor(
            Duration delay,
            std::stop_token token = {},
            std::shared_ptr<IExecutor> executor = {});

        SleepAwaiter sleepUntil(
            TimePoint deadline,
            std::stop_token token = {},
            std::shared_ptr<IExecutor> executor = {});

        // -- manual driving -----------------------------------------

        // Dispatches every timer that is due according to the clock, right
        // now, on the calling thread (the callbacks themselves still run
        // on their executors). One pass: timers that become due because
        // of what this pass runs wait for the next call. Returns how many
        // were dispatched. Meant for SchedulerOptions::useTimerThread ==
        // false, but harmless otherwise.
        std::size_t runDue();

        // Deadline of the earliest pending timer, if any.
        std::optional<TimePoint> nextDeadline() const;

        // -- state --------------------------------------------------

        TimePoint now() const;
        const std::shared_ptr<IClock>& clock() const noexcept;

        // The default executor (created on first use). After stop() it
        // rejects everything.
        std::shared_ptr<IExecutor> executor();

        void stop();
        bool stopped() const noexcept;

    private:
        friend class SleepAwaiter;

        std::shared_ptr<detail::SchedulerImpl> m_impl;
    };
}
