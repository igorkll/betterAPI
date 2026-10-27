#include <dsbox_scheduler/scheduler.hpp>
#include <dsbox_scheduler/timer_queue.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dsbox_scheduler
{
    namespace detail
    {
        // What a timer does, fixed at creation. Immutable afterwards,
        // except that a one-shot timer's callbacks are released once it
        // has run or been cancelled (see runTimer()/TimerHandle::cancel()).
        struct TimerSpec
        {
            Callback task;
            Callback onDrop; // one-shot only
            std::shared_ptr<IExecutor> executor;
            Duration period{ 0 }; // 0 = one-shot
            PeriodicPolicy policy = PeriodicPolicy::FixedRate;
            ErrorHandler onError;
        };

        // Shared between the queue entries, the running callbacks and
        // every TimerHandle copy.
        struct TimerState
        {
            // One-shot: cleared by whoever wins the race between "start
            // running" and cancel(). Periodic: cleared by cancel().
            std::atomic<bool> active{ true };

            // Runs currently in flight, for cancelAndWait().
            std::atomic<int> running{ 0 };
            std::mutex mutex;
            std::condition_variable cv;

            std::weak_ptr<SchedulerImpl> core;

            TimerId id = 0; // id of the CURRENT queue entry; guarded by SchedulerImpl::mutex
            TimerSpec spec;
        };

        // The timer whose callback this thread is running, so that
        // cancelAndWait() from inside it does not wait for itself.
        thread_local const TimerState* t_currentTimer = nullptr;

        class RejectingExecutor final : public IExecutor
        {
        public:
            bool post(Callback) override
            {
                return false;
            }
        };

        void runTimer(const std::shared_ptr<TimerState>& state, TimePoint scheduled);

        struct SchedulerImpl : std::enable_shared_from_this<SchedulerImpl>
        {
            std::shared_ptr<IClock> clock;
            ErrorHandler onError;
            std::size_t poolThreads = 0;
            bool useThread = true;

            mutable std::mutex mutex; // guards queue, thread, threadStarted, stopping
            std::condition_variable cv;
            TimerQueue queue;
            std::thread thread;
            bool threadStarted = false;
            bool stopping = false;
            std::atomic<bool> stopped{ false };

            // Never held together with `mutex`.
            std::mutex executorMutex;
            std::shared_ptr<IExecutor> defaultExecutor;
            std::shared_ptr<ThreadPoolExecutor> ownedPool;

            std::shared_ptr<IExecutor> executor()
            {
                std::lock_guard lock(executorMutex);

                if (!defaultExecutor)
                {
                    ownedPool = std::make_shared<ThreadPoolExecutor>(poolThreads, onError);
                    defaultExecutor = ownedPool;
                }

                return defaultExecutor;
            }

            void reportError(std::exception_ptr error) const noexcept
            {
                if (!onError)
                    return;

                try
                {
                    onError(std::move(error));
                }
                catch (...)
                {
                }
            }

            TimerEntry makeEntry(const std::shared_ptr<TimerState>& state, TimePoint deadline) const
            {
                TimerEntry entry;
                entry.deadline = deadline;
                entry.executor = state->spec.executor;
                entry.onDrop = state->spec.onDrop;
                entry.task = [state, deadline] { runTimer(state, deadline); };
                return entry;
            }

            // Registers a new timer. Returns an empty handle if stopped.
            TimerHandle schedule(TimePoint deadline, TimerSpec spec)
            {
                if (!spec.executor)
                    spec.executor = executor();

                auto state = std::make_shared<TimerState>();
                state->core = weak_from_this();
                state->spec = std::move(spec);

                {
                    std::lock_guard lock(mutex);

                    if (stopping)
                        return {};

                    state->id = queue.add(makeEntry(state, deadline));

                    if (useThread && !threadStarted)
                    {
                        threadStarted = true;
                        thread = std::thread([self = shared_from_this()] { self->timerLoop(); });
                    }
                }

                cv.notify_all();
                return TimerHandle(std::move(state));
            }

            // Queues the next run of a periodic timer.
            void rearm(const std::shared_ptr<TimerState>& state, TimePoint deadline)
            {
                {
                    std::lock_guard lock(mutex);

                    if (stopping || !state->active.load())
                        return;

                    state->id = queue.add(makeEntry(state, deadline));
                }

                cv.notify_all();
            }

            void dispatch(std::vector<TimerEntry>& due)
            {
                for (auto& entry : due)
                {
                    bool accepted = false;

                    try
                    {
                        accepted = entry.executor->post(std::move(entry.task));
                    }
                    catch (...)
                    {
                        reportError(std::current_exception());
                    }

                    if (!accepted && entry.onDrop)
                        invokeGuarded(entry.onDrop, onError);
                }
            }

            void timerLoop()
            {
                std::unique_lock lock(mutex);

                while (!stopping)
                {
                    const auto next = queue.nextDeadline();

                    if (!next)
                    {
                        cv.wait(lock);
                        continue;
                    }

                    const TimePoint now = clock->now();

                    if (*next > now)
                    {
                        // Capped so an enormous deadline cannot overflow
                        // the condition variable's own clock arithmetic.
                        cv.wait_for(lock, std::min<Duration>(*next - now, std::chrono::hours(1)));
                        continue;
                    }

                    auto due = queue.popDue(now);

                    lock.unlock();
                    dispatch(due);
                    lock.lock();
                }
            }

            std::size_t runDue()
            {
                std::vector<TimerEntry> due;

                {
                    std::lock_guard lock(mutex);

                    if (stopping)
                        return 0;

                    due = queue.popDue(clock->now());
                }

                dispatch(due);
                return due.size();
            }

            void stop()
            {
                std::vector<TimerEntry> dropped;
                std::thread worker;

                {
                    std::lock_guard lock(mutex);

                    if (stopping)
                        return;

                    stopping = true;
                    dropped = queue.drain();
                    worker = std::move(thread);
                }

                stopped.store(true);
                cv.notify_all();

                if (worker.joinable())
                {
                    // A callback running inline on the timer thread (an
                    // InlineExecutor) may itself call stop().
                    if (worker.get_id() == std::this_thread::get_id())
                        worker.detach();
                    else
                        worker.join();
                }

                // Tell whoever was waiting on a discarded timer (sleeping
                // coroutines) -- through its own executor if that still
                // accepts work, otherwise right here.
                for (auto& entry : dropped)
                {
                    if (!entry.onDrop)
                        continue;

                    const Callback drop = entry.onDrop;

                    bool posted = false;

                    try
                    {
                        posted = entry.executor && entry.executor->post(drop);
                    }
                    catch (...)
                    {
                        reportError(std::current_exception());
                    }

                    if (!posted)
                        invokeGuarded(drop, onError);
                }

                std::shared_ptr<ThreadPoolExecutor> pool;

                {
                    std::lock_guard lock(executorMutex);

                    // From now on executor() hands out something that
                    // rejects, instead of lazily creating a fresh pool.
                    if (!defaultExecutor)
                        defaultExecutor = std::make_shared<RejectingExecutor>();

                    pool = ownedPool;
                }

                if (pool)
                    pool->shutdown();
            }
        };

        struct RunningScope
        {
            explicit RunningScope(TimerState& s) noexcept
                : state(s)
            {
                state.running.fetch_add(1);
            }

            ~RunningScope()
            {
                if (state.running.fetch_sub(1) == 1)
                {
                    std::lock_guard lock(state.mutex);
                    state.cv.notify_all();
                }
            }

            TimerState& state;
        };

        struct CurrentTimerScope
        {
            explicit CurrentTimerScope(const TimerState* s) noexcept
                : previous(t_currentTimer)
            {
                t_currentTimer = s;
            }

            ~CurrentTimerScope()
            {
                t_currentTimer = previous;
            }

            const TimerState* previous;
        };

        // Runs on the timer's executor. `scheduled` is the deadline this
        // particular queue entry was scheduled for.
        void runTimer(const std::shared_ptr<TimerState>& state, TimePoint scheduled)
        {
            RunningScope running(*state);

            TimerSpec& spec = state->spec;
            const bool periodic = spec.period.count() > 0;

            if (!periodic)
            {
                // Claim the single run; loses against a cancel().
                if (!state->active.exchange(false))
                    return;
            }
            else if (!state->active.load())
            {
                return;
            }

            const std::shared_ptr<SchedulerImpl> impl = state->core.lock();

            if (periodic && impl && spec.policy == PeriodicPolicy::FixedRate)
            {
                TimePoint next = scheduled + spec.period;
                const TimePoint now = impl->clock->now();

                if (next <= now)
                {
                    // Skip the ticks that were missed instead of firing
                    // them all back to back.
                    const auto missed = (now - next) / spec.period + 1;
                    next += spec.period * missed;
                }

                impl->rearm(state, next);
            }

            {
                CurrentTimerScope current(state.get());
                invokeGuarded(spec.task, spec.onError);
            }

            if (!periodic)
            {
                // Only this run ever touched them; release captures now
                // rather than when the last handle goes away.
                spec.task = nullptr;
                spec.onDrop = nullptr;
            }
            else if (impl && spec.policy == PeriodicPolicy::FixedDelay)
            {
                impl->rearm(state, impl->clock->now() + spec.period);
            }
        }

        struct SleepState
        {
            std::coroutine_handle<> handle;
            std::atomic<bool> claimed{ false }; // whoever sets it resumes the coroutine
            bool result = false;
            TimerHandle timer;
            std::shared_ptr<IExecutor> executor;
            std::optional<std::stop_callback<std::function<void()>>> stopCallback;
        };

        // The timer fired (result = true) or was discarded by stop()
        // (result = false). Runs on the sleep's executor, or inline as
        // the fallback described in SchedulerImpl::stop().
        void finishSleep(SleepState& state, bool result)
        {
            if (state.claimed.exchange(true))
                return;

            state.result = result;
            state.handle.resume();
        }

        // The stop_token fired, on whatever thread requested stop.
        void abortSleep(SleepState& state)
        {
            if (state.claimed.exchange(true))
                return;

            state.result = false;
            state.timer.cancel();

            const std::coroutine_handle<> handle = state.handle;

            if (!(state.executor && state.executor->post([handle] { handle.resume(); })))
                handle.resume();
        }

        TimePoint addSaturating(TimePoint base, Duration delta)
        {
            if (delta > Duration::zero() && delta > TimePoint::max() - base)
                return TimePoint::max();

            return base + delta;
        }
    }

    // ---------------------------------------------------------------
    // TimerHandle
    // ---------------------------------------------------------------

    bool TimerHandle::active() const noexcept
    {
        if (!m_state || !m_state->active.load())
            return false;

        const auto impl = m_state->core.lock();
        return impl && !impl->stopped.load();
    }

    bool TimerHandle::cancel()
    {
        if (!m_state)
            return false;

        const bool wasActive = m_state->active.exchange(false);

        if (const auto impl = m_state->core.lock())
        {
            std::lock_guard lock(impl->mutex);
            impl->queue.cancel(m_state->id);
        }

        // If cancel won the race against a one-shot's run, that run will
        // never happen: release its captures now.
        if (wasActive && m_state->spec.period.count() == 0)
        {
            m_state->spec.task = nullptr;
            m_state->spec.onDrop = nullptr;
        }

        return wasActive;
    }

    void TimerHandle::cancelAndWait()
    {
        if (!m_state)
            return;

        cancel();

        if (detail::t_currentTimer == m_state.get())
            return;

        std::unique_lock lock(m_state->mutex);
        m_state->cv.wait(lock, [this] { return m_state->running.load() == 0; });
    }

    // ---------------------------------------------------------------
    // SleepAwaiter
    // ---------------------------------------------------------------

    SleepAwaiter::SleepAwaiter(
        Scheduler& scheduler,
        TimePoint deadline,
        std::stop_token token,
        std::shared_ptr<IExecutor> executor)
        : m_scheduler(&scheduler)
        , m_deadline(deadline)
        , m_token(std::move(token))
        , m_executor(std::move(executor))
    {
    }

    bool SleepAwaiter::await_suspend(std::coroutine_handle<> handle)
    {
        // Everything read from `this` happens before the timer is
        // scheduled: once it is, the coroutine may be resumed (and this
        // awaiter destroyed) on another thread while we are still in here.
        auto state = std::make_shared<detail::SleepState>();
        state->handle = handle;
        m_state = state;

        const std::stop_token token = m_token;

        if (token.stop_requested())
        {
            state->result = false;
            return false;
        }

        detail::SchedulerImpl& impl = *m_scheduler->m_impl;
        auto executor = m_executor ? m_executor : impl.executor();
        state->executor = executor;

        // Weak on purpose: the state owns the timer handle, and the timer
        // owns these callbacks -- a strong reference here would be a cycle.
        // If the coroutine is destroyed while suspended, the state dies
        // with it and a late timer becomes a no-op.
        const std::weak_ptr<detail::SleepState> weak = state;

        detail::TimerSpec spec;
        spec.executor = executor;
        spec.onError = impl.onError;
        spec.task = [weak]
        {
            if (const auto s = weak.lock())
                detail::finishSleep(*s, true);
        };
        spec.onDrop = [weak]
        {
            if (const auto s = weak.lock())
                detail::finishSleep(*s, false);
        };

        TimerHandle timer = impl.schedule(m_deadline, std::move(spec));

        if (!timer)
        {
            state->result = false; // scheduler already stopped
            return false;
        }

        state->timer = std::move(timer);

        if (token.stop_possible())
        {
            state->stopCallback.emplace(
                token,
                std::function<void()>(
                    [weak]
                    {
                        if (const auto s = weak.lock())
                            detail::abortSleep(*s);
                    }));
        }

        return true;
    }

    bool SleepAwaiter::await_resume()
    {
        const auto state = std::move(m_state);

        if (!state)
            return false;

        state->stopCallback.reset();
        return state->result;
    }

    // ---------------------------------------------------------------
    // Scheduler
    // ---------------------------------------------------------------

    Scheduler::Scheduler(SchedulerOptions options)
        : m_impl(std::make_shared<detail::SchedulerImpl>())
    {
        detail::SchedulerImpl& impl = *m_impl;

        if (options.clock)
            impl.clock = std::move(options.clock);
        else
            impl.clock = std::make_shared<SteadyClock>();

        impl.onError = std::move(options.errorHandler);
        impl.poolThreads = options.threads;
        impl.useThread = options.useTimerThread;
        impl.defaultExecutor = std::move(options.defaultExecutor);
    }

    Scheduler::~Scheduler()
    {
        if (m_impl)
            m_impl->stop();
    }

    TimerHandle Scheduler::at(TimePoint when, Callback task, std::shared_ptr<IExecutor> executor)
    {
        if (!task)
            throw std::invalid_argument("dsbox_scheduler: null task");

        detail::TimerSpec spec;
        spec.task = std::move(task);
        spec.executor = std::move(executor);
        spec.onError = m_impl->onError;

        return m_impl->schedule(when, std::move(spec));
    }

    TimerHandle Scheduler::after(Duration delay, Callback task, std::shared_ptr<IExecutor> executor)
    {
        return at(detail::addSaturating(now(), delay), std::move(task), std::move(executor));
    }

    TimerHandle Scheduler::every(
        Duration period,
        Callback task,
        PeriodicPolicy policy,
        std::shared_ptr<IExecutor> executor)
    {
        return everyAt(
            detail::addSaturating(now(), period),
            period,
            std::move(task),
            policy,
            std::move(executor));
    }

    TimerHandle Scheduler::everyAt(
        TimePoint first,
        Duration period,
        Callback task,
        PeriodicPolicy policy,
        std::shared_ptr<IExecutor> executor)
    {
        if (period <= Duration::zero())
            throw std::invalid_argument("dsbox_scheduler: period must be > 0");

        if (!task)
            throw std::invalid_argument("dsbox_scheduler: null task");

        detail::TimerSpec spec;
        spec.task = std::move(task);
        spec.executor = std::move(executor);
        spec.period = period;
        spec.policy = policy;
        spec.onError = m_impl->onError;

        return m_impl->schedule(first, std::move(spec));
    }

    bool Scheduler::post(Callback task, std::shared_ptr<IExecutor> executor)
    {
        if (!task || m_impl->stopped.load())
            return false;

        if (!executor)
            executor = m_impl->executor();

        return executor->post(std::move(task));
    }

    SleepAwaiter Scheduler::sleepFor(Duration delay, std::stop_token token, std::shared_ptr<IExecutor> executor)
    {
        return sleepUntil(detail::addSaturating(now(), delay), std::move(token), std::move(executor));
    }

    SleepAwaiter Scheduler::sleepUntil(TimePoint deadline, std::stop_token token, std::shared_ptr<IExecutor> executor)
    {
        return SleepAwaiter(*this, deadline, std::move(token), std::move(executor));
    }

    std::size_t Scheduler::runDue()
    {
        return m_impl->runDue();
    }

    std::optional<TimePoint> Scheduler::nextDeadline() const
    {
        std::lock_guard lock(m_impl->mutex);
        return m_impl->queue.nextDeadline();
    }

    TimePoint Scheduler::now() const
    {
        return m_impl->clock->now();
    }

    const std::shared_ptr<IClock>& Scheduler::clock() const noexcept
    {
        return m_impl->clock;
    }

    std::shared_ptr<IExecutor> Scheduler::executor()
    {
        return m_impl->executor();
    }

    void Scheduler::stop()
    {
        m_impl->stop();
    }

    bool Scheduler::stopped() const noexcept
    {
        return m_impl->stopped.load();
    }
}
