#pragma once

#include <dsbox_scheduler/types.hpp>

#include <atomic>

namespace dsbox_scheduler
{
    // Source of "now" for a Scheduler. Exists so tests (and any host with
    // its own notion of time) can drive timers deterministically instead
    // of sleeping in real time.
    class IClock
    {
    public:
        virtual ~IClock() = default;

        virtual TimePoint now() const = 0;
    };

    class SteadyClock final : public IClock
    {
    public:
        TimePoint now() const override
        {
            return Clock::now();
        }
    };

    // A clock that only moves when told to. Thread-safe.
    //
    // Pair it with SchedulerOptions::useTimerThread = false and drive the
    // scheduler with Scheduler::runDue() after each advance():
    //
    //   auto clock = std::make_shared<ManualClock>();
    //   Scheduler scheduler({ .clock = clock, .useTimerThread = false });
    //   scheduler.after(10ms, task);
    //   clock->advance(10ms);
    //   scheduler.runDue();   // task is dispatched now
    //
    // (A scheduler WITH a timer thread waits in real time and would not
    // notice a ManualClock moving.)
    class ManualClock final : public IClock
    {
    public:
        explicit ManualClock(TimePoint start = TimePoint{}) noexcept
            : m_ticks(start.time_since_epoch().count())
        {
        }

        TimePoint now() const override
        {
            return TimePoint(Duration(m_ticks.load(std::memory_order_acquire)));
        }

        void advance(Duration by) noexcept
        {
            m_ticks.fetch_add(by.count(), std::memory_order_acq_rel);
        }

        void set(TimePoint to) noexcept
        {
            m_ticks.store(to.time_since_epoch().count(), std::memory_order_release);
        }

    private:
        std::atomic<Duration::rep> m_ticks;
    };
}
