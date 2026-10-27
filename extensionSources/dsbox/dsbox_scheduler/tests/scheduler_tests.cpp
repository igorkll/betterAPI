#include "test_framework.hpp"

#include <dsbox_scheduler/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dsbox_scheduler;
using namespace std::chrono_literals;

namespace
{
    // Deterministic scheduler: manual clock, no timer thread, callbacks
    // run inline inside runDue(). Nothing here sleeps.
    struct Manual
    {
        std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
        std::shared_ptr<IExecutor> inlineExecutor = std::make_shared<InlineExecutor>();
        Scheduler scheduler;

        explicit Manual(ErrorHandler onError = {})
            : scheduler(makeOptions(clock, inlineExecutor, std::move(onError)))
        {
        }

        void advance(Duration by)
        {
            clock->advance(by);
            scheduler.runDue();
        }

        static SchedulerOptions makeOptions(
            std::shared_ptr<ManualClock> clock,
            std::shared_ptr<IExecutor> executor,
            ErrorHandler onError)
        {
            SchedulerOptions options;
            options.clock = std::move(clock);
            options.defaultExecutor = std::move(executor);
            options.useTimerThread = false;
            options.errorHandler = std::move(onError);
            return options;
        }
    };

    void testAfterFiresOnlyWhenDue()
    {
        Manual m;
        int count = 0;

        m.scheduler.after(10ms, [&] { ++count; });

        m.advance(9ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);

        m.advance(1ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);

        m.advance(1s); // one-shot: never again
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
    }

    void testAtInThePastFiresOnNextRun()
    {
        Manual m;
        m.clock->advance(1s);
        int count = 0;

        m.scheduler.at(m.scheduler.now() - 500ms, [&] { ++count; });
        DSBOX_SCHEDULER_CHECK_EQ(count, 0); // still only queued, not run inline
        DSBOX_SCHEDULER_CHECK_EQ(m.scheduler.runDue(), 1u);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
    }

    void testOrderingByDeadlineThenFifo()
    {
        Manual m;
        std::vector<int> order;

        m.scheduler.after(30ms, [&] { order.push_back(3); });
        m.scheduler.after(10ms, [&] { order.push_back(1); });
        m.scheduler.after(10ms, [&] { order.push_back(2); });

        m.advance(1s);
        DSBOX_SCHEDULER_CHECK((order == std::vector<int>{ 1, 2, 3 }));
    }

    void testNextDeadline()
    {
        Manual m;
        DSBOX_SCHEDULER_CHECK(!m.scheduler.nextDeadline().has_value());

        m.scheduler.after(20ms, [] {});
        m.scheduler.after(5ms, [] {});
        DSBOX_SCHEDULER_CHECK(m.scheduler.nextDeadline() == m.scheduler.now() + 5ms);
    }

    void testCancelPreventsRun()
    {
        Manual m;
        int count = 0;

        TimerHandle handle = m.scheduler.after(10ms, [&] { ++count; });
        DSBOX_SCHEDULER_CHECK(handle);
        DSBOX_SCHEDULER_CHECK(handle.active());

        DSBOX_SCHEDULER_CHECK(handle.cancel());
        DSBOX_SCHEDULER_CHECK(!handle.active());
        DSBOX_SCHEDULER_CHECK(!handle.cancel()); // second time: nothing to cancel

        m.advance(1s);
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);
    }

    void testCancelAfterRunReturnsFalse()
    {
        Manual m;
        int count = 0;

        TimerHandle handle = m.scheduler.after(10ms, [&] { ++count; });
        m.advance(10ms);

        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
        DSBOX_SCHEDULER_CHECK(!handle.active());
        DSBOX_SCHEDULER_CHECK(!handle.cancel());
    }

    void testCancelStopsQueuedButNotYetStartedRun()
    {
        // The timer is due and handed to a loop executor, but the loop has
        // not run it yet: cancel() must still prevent it.
        Manual m;
        LoopExecutor loop;
        int count = 0;

        TimerHandle handle = m.scheduler.after(10ms, [&] { ++count; }, nonOwning(loop));
        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK_EQ(loop.pending(), 1u);
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);

        DSBOX_SCHEDULER_CHECK(handle.cancel());
        loop.poll();
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);
    }

    void testEmptyHandleIsHarmless()
    {
        TimerHandle handle;
        DSBOX_SCHEDULER_CHECK(!handle);
        DSBOX_SCHEDULER_CHECK(!handle.active());
        DSBOX_SCHEDULER_CHECK(!handle.cancel());
        handle.cancelAndWait();
    }

    void testRunsOnRequestedExecutor()
    {
        Manual m;
        LoopExecutor loop;
        int count = 0;

        m.scheduler.after(10ms, [&] { ++count; }, nonOwning(loop));
        m.advance(10ms);

        DSBOX_SCHEDULER_CHECK_EQ(count, 0); // dispatched, but only the loop's owner can run it
        DSBOX_SCHEDULER_CHECK_EQ(loop.poll(), 1u);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
    }

    void testFixedRate()
    {
        Manual m;
        int count = 0;

        m.scheduler.every(10ms, [&] { ++count; });

        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 2);
        m.advance(5ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 2);
        m.advance(5ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 3);
    }

    void testFixedRateSkipsMissedTicks()
    {
        Manual m;
        int count = 0;

        m.scheduler.every(10ms, [&] { ++count; }); // due at 10, 20, 30, ...

        m.advance(35ms); // ticks 10, 20, 30 were all due
        DSBOX_SCHEDULER_CHECK_EQ(count, 1); // one run, not a burst of three
        DSBOX_SCHEDULER_CHECK(m.scheduler.nextDeadline() == TimePoint{} + 40ms); // and the grid is kept

        m.advance(5ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 2);
    }

    void testFixedDelayCountsFromCompletion()
    {
        Manual m;
        int count = 0;

        m.scheduler.every(10ms, [&] { ++count; }, PeriodicPolicy::FixedDelay);

        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);

        // Ran at t=10, so the next run is due at 20 -- not before.
        DSBOX_SCHEDULER_CHECK(m.scheduler.nextDeadline() == TimePoint{} + 20ms);

        m.advance(35ms); // now t=45, run at 45 -> next at 55 (no catch-up)
        DSBOX_SCHEDULER_CHECK_EQ(count, 2);
        DSBOX_SCHEDULER_CHECK(m.scheduler.nextDeadline() == TimePoint{} + 55ms);
    }

    void testCancelPeriodic()
    {
        Manual m;
        int count = 0;

        TimerHandle handle = m.scheduler.every(10ms, [&] { ++count; });
        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);

        DSBOX_SCHEDULER_CHECK(handle.cancel());
        m.advance(100ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
        DSBOX_SCHEDULER_CHECK(!m.scheduler.nextDeadline().has_value());
    }

    void testPeriodicCancelsItselfFromItsCallback()
    {
        Manual m;
        int count = 0;
        TimerHandle handle;

        handle = m.scheduler.every(
            10ms,
            [&]
            {
                if (++count == 3)
                    handle.cancel();
            });

        m.advance(10ms);
        m.advance(10ms);
        m.advance(10ms);
        m.advance(10ms);
        m.advance(10ms);

        DSBOX_SCHEDULER_CHECK_EQ(count, 3);
        DSBOX_SCHEDULER_CHECK(!m.scheduler.nextDeadline().has_value());
    }

    void testFixedDelayCancelsItselfFromItsCallback()
    {
        Manual m;
        int count = 0;
        TimerHandle handle;

        handle = m.scheduler.every(
            10ms,
            [&]
            {
                ++count;
                handle.cancel();
            },
            PeriodicPolicy::FixedDelay);

        m.advance(10ms);
        m.advance(100ms);

        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
        DSBOX_SCHEDULER_CHECK(!m.scheduler.nextDeadline().has_value());
    }

    void testScopedTimerCancelsOnDestruction()
    {
        Manual m;
        int count = 0;

        {
            ScopedTimer timer(m.scheduler.after(10ms, [&] { ++count; }));
            DSBOX_SCHEDULER_CHECK(timer.active());
        }

        m.advance(1s);
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);
    }

    void testScopedTimerReleaseAndReset()
    {
        Manual m;
        int a = 0;
        int b = 0;

        {
            ScopedTimer timer(m.scheduler.after(10ms, [&] { ++a; }));
            TimerHandle released = timer.release(); // ownership gone: no cancel on destruction
            (void)released;
        }

        ScopedTimer timer(m.scheduler.after(10ms, [&] { ++b; }));
        timer.reset(); // cancels

        m.advance(1s);
        DSBOX_SCHEDULER_CHECK_EQ(a, 1);
        DSBOX_SCHEDULER_CHECK_EQ(b, 0);
    }

    void testScopedTimerMove()
    {
        Manual m;
        int count = 0;

        ScopedTimer outer;
        {
            ScopedTimer inner(m.scheduler.after(10ms, [&] { ++count; }));
            outer = std::move(inner);
        } // inner is empty now: destroying it must not cancel

        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
    }

    void testPostRunsOnDefaultExecutor()
    {
        Manual m;
        bool ran = false;

        DSBOX_SCHEDULER_CHECK(m.scheduler.post([&] { ran = true; }));
        DSBOX_SCHEDULER_CHECK(ran); // inline executor
    }

    void testExceptionsGoToErrorHandler()
    {
        int errors = 0;
        Manual m([&](std::exception_ptr) { ++errors; });
        int later = 0;

        m.scheduler.after(10ms, [] { throw std::runtime_error("boom"); });
        m.scheduler.after(20ms, [&] { ++later; });

        m.advance(20ms);
        DSBOX_SCHEDULER_CHECK_EQ(errors, 1);
        DSBOX_SCHEDULER_CHECK_EQ(later, 1);
    }

    void testInvalidArguments()
    {
        Manual m;
        bool threw = false;

        try
        {
            m.scheduler.every(0ms, [] {});
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        DSBOX_SCHEDULER_CHECK(threw);

        threw = false;
        try
        {
            m.scheduler.after(1ms, Callback{});
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        DSBOX_SCHEDULER_CHECK(threw);
    }

    void testStopDiscardsPendingAndRejectsNew()
    {
        Manual m;
        int count = 0;

        TimerHandle pending = m.scheduler.after(10ms, [&] { ++count; });
        TimerHandle periodic = m.scheduler.every(10ms, [&] { ++count; });

        m.scheduler.stop();
        DSBOX_SCHEDULER_CHECK(m.scheduler.stopped());
        DSBOX_SCHEDULER_CHECK(!pending.active());
        DSBOX_SCHEDULER_CHECK(!periodic.active());

        m.clock->advance(1s);
        DSBOX_SCHEDULER_CHECK_EQ(m.scheduler.runDue(), 0u);
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);

        TimerHandle after = m.scheduler.after(1ms, [&] { ++count; });
        DSBOX_SCHEDULER_CHECK(!after); // empty handle
        DSBOX_SCHEDULER_CHECK(!m.scheduler.post([&] { ++count; }));
        DSBOX_SCHEDULER_CHECK_EQ(count, 0);

        m.scheduler.stop(); // idempotent
    }

    void testStopFromInsideACallback()
    {
        Manual m;
        int count = 0;

        m.scheduler.after(10ms, [&] { m.scheduler.stop(); ++count; });
        m.scheduler.after(20ms, [&] { ++count; });

        m.advance(10ms);
        m.advance(100ms);

        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
    }

    // -- real time, real threads (short waits only) -------------------------

    void testTimerThreadFiresAfterDelay()
    {
        Scheduler scheduler;
        std::atomic<bool> fired{ false };

        const auto start = std::chrono::steady_clock::now();
        scheduler.after(30ms, [&] { fired = true; });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return fired.load(); }));
        DSBOX_SCHEDULER_CHECK(std::chrono::steady_clock::now() - start >= 25ms);
    }

    void testEarlierTimerOvertakesLaterOne()
    {
        // The thread is already sleeping towards the 10s timer when a
        // 10ms one arrives: it has to wake up and re-plan.
        Scheduler scheduler;
        std::atomic<bool> fired{ false };

        scheduler.after(10s, [] {});
        std::this_thread::sleep_for(20ms);
        scheduler.after(10ms, [&] { fired = true; });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return fired.load(); }, 2000ms));
    }

    void testPeriodicOnRealThread()
    {
        Scheduler scheduler;
        std::atomic<int> count{ 0 };

        TimerHandle handle = scheduler.every(5ms, [&] { ++count; });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return count.load() >= 5; }));
        handle.cancelAndWait();

        const int after = count.load();
        std::this_thread::sleep_for(30ms);
        DSBOX_SCHEDULER_CHECK_EQ(count.load(), after); // really stopped
    }

    void testCancelAndWaitBlocksUntilCallbackFinishes()
    {
        Scheduler scheduler;
        std::atomic<bool> started{ false };
        std::atomic<bool> finished{ false };

        TimerHandle handle = scheduler.after(
            1ms,
            [&]
            {
                started = true;
                std::this_thread::sleep_for(60ms);
                finished = true;
            });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return started.load(); }));

        handle.cancelAndWait();
        DSBOX_SCHEDULER_CHECK(finished.load()); // it did not return while the callback was still running
    }

    void testCancelAndWaitFromOwnCallbackDoesNotDeadlock()
    {
        Scheduler scheduler;
        std::atomic<bool> done{ false };
        std::atomic<bool> assigned{ false };
        auto self = std::make_shared<TimerHandle>();
        const std::weak_ptr<TimerHandle> weak = self; // a strong capture would be a cycle

        *self = scheduler.every(
            2ms,
            [weak, &done, &assigned]
            {
                // The callback may fire before the test thread has stored
                // the handle; `assigned` publishes it.
                if (!assigned.load())
                    return;

                if (const auto handle = weak.lock())
                {
                    handle->cancelAndWait();
                    done = true;
                }
            });
        assigned = true;

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return done.load(); }));
    }

    void testDestructorWithPendingTimersDoesNotHang()
    {
        std::atomic<int> count{ 0 };

        {
            Scheduler scheduler;
            scheduler.after(10s, [&] { ++count; });
            scheduler.every(10s, [&] { ++count; });
        }

        DSBOX_SCHEDULER_CHECK_EQ(count.load(), 0);
    }

    void testStopFromTimerThreadCallbackWithInlineExecutor()
    {
        // With an InlineExecutor the callback runs on the timer thread
        // itself; stop() from there must not try to join itself.
        SchedulerOptions options;
        options.defaultExecutor = std::make_shared<InlineExecutor>();

        auto scheduler = std::make_shared<Scheduler>(std::move(options));
        std::atomic<bool> done{ false };

        scheduler->after(
            5ms,
            [&, raw = scheduler.get()]
            {
                raw->stop();
                done = true;
            });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return done.load(); }));
        scheduler.reset();
    }

    void testOwnedPoolIsShutDownByStop()
    {
        Scheduler scheduler;
        std::atomic<int> count{ 0 };

        DSBOX_SCHEDULER_CHECK(scheduler.post([&] { ++count; }));
        scheduler.stop();

        DSBOX_SCHEDULER_CHECK(!scheduler.executor()->post([&] { ++count; }));
        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return count.load() == 1; }));
    }

    void testStopOnUnusedSchedulerRejectsExecutorToo()
    {
        Scheduler scheduler;
        scheduler.stop();

        // No pool was ever created: executor() must not conjure a live one now.
        DSBOX_SCHEDULER_CHECK(!scheduler.executor()->post([] {}));
    }
}

void runSchedulerTests()
{
    using dsbox_scheduler_tests::runTest;

    runTest("Scheduler: after fires only when due", testAfterFiresOnlyWhenDue);
    runTest("Scheduler: at() in the past fires on next run", testAtInThePastFiresOnNextRun);
    runTest("Scheduler: deadline order, then FIFO", testOrderingByDeadlineThenFifo);
    runTest("Scheduler: nextDeadline", testNextDeadline);
    runTest("Scheduler: cancel prevents run", testCancelPreventsRun);
    runTest("Scheduler: cancel after run returns false", testCancelAfterRunReturnsFalse);
    runTest("Scheduler: cancel stops a queued-but-not-started run", testCancelStopsQueuedButNotYetStartedRun);
    runTest("Scheduler: empty handle is harmless", testEmptyHandleIsHarmless);
    runTest("Scheduler: runs on the requested executor", testRunsOnRequestedExecutor);
    runTest("Scheduler: FixedRate", testFixedRate);
    runTest("Scheduler: FixedRate skips missed ticks", testFixedRateSkipsMissedTicks);
    runTest("Scheduler: FixedDelay counts from completion", testFixedDelayCountsFromCompletion);
    runTest("Scheduler: cancel periodic", testCancelPeriodic);
    runTest("Scheduler: FixedRate cancels itself from its callback", testPeriodicCancelsItselfFromItsCallback);
    runTest("Scheduler: FixedDelay cancels itself from its callback", testFixedDelayCancelsItselfFromItsCallback);
    runTest("ScopedTimer: cancels on destruction", testScopedTimerCancelsOnDestruction);
    runTest("ScopedTimer: release / reset", testScopedTimerReleaseAndReset);
    runTest("ScopedTimer: move", testScopedTimerMove);
    runTest("Scheduler: post runs on default executor", testPostRunsOnDefaultExecutor);
    runTest("Scheduler: exceptions go to the error handler", testExceptionsGoToErrorHandler);
    runTest("Scheduler: invalid arguments", testInvalidArguments);
    runTest("Scheduler: stop discards pending, rejects new", testStopDiscardsPendingAndRejectsNew);
    runTest("Scheduler: stop from inside a callback", testStopFromInsideACallback);
    runTest("Scheduler[thread]: fires after the delay", testTimerThreadFiresAfterDelay);
    runTest("Scheduler[thread]: earlier timer overtakes later one", testEarlierTimerOvertakesLaterOne);
    runTest("Scheduler[thread]: periodic", testPeriodicOnRealThread);
    runTest("Scheduler[thread]: cancelAndWait blocks until callback ends", testCancelAndWaitBlocksUntilCallbackFinishes);
    runTest("Scheduler[thread]: cancelAndWait from own callback", testCancelAndWaitFromOwnCallbackDoesNotDeadlock);
    runTest("Scheduler[thread]: destructor with pending timers", testDestructorWithPendingTimersDoesNotHang);
    runTest("Scheduler[thread]: stop() from timer-thread callback", testStopFromTimerThreadCallbackWithInlineExecutor);
    runTest("Scheduler[thread]: stop shuts the owned pool", testOwnedPoolIsShutDownByStop);
    runTest("Scheduler[thread]: stopped-unused scheduler rejects", testStopOnUnusedSchedulerRejectsExecutorToo);
}
