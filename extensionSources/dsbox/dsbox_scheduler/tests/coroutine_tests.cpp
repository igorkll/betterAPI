#include "test_framework.hpp"

#include <dsbox_scheduler/coroutines.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

using namespace dsbox_scheduler;
using namespace std::chrono_literals;

namespace
{
    // Same deterministic setup as in scheduler_tests.cpp: manual clock, no
    // timer thread, everything inline.
    struct Manual
    {
        std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
        std::shared_ptr<IExecutor> inlineExecutor = std::make_shared<InlineExecutor>();
        Scheduler scheduler;

        Manual()
            : scheduler(makeOptions(clock, inlineExecutor))
        {
        }

        void advance(Duration by)
        {
            clock->advance(by);
            scheduler.runDue();
        }

        static SchedulerOptions makeOptions(std::shared_ptr<ManualClock> clock, std::shared_ptr<IExecutor> executor)
        {
            SchedulerOptions options;
            options.clock = std::move(clock);
            options.defaultExecutor = std::move(executor);
            options.useTimerThread = false;
            return options;
        }
    };

    template<typename T>
    bool isReady(std::future<T>& future)
    {
        return future.wait_for(0ms) == std::future_status::ready;
    }

    // -- Task ---------------------------------------------------------------

    Task<int> answer()
    {
        co_return 42;
    }

    Task<int> addOne(int value)
    {
        const int base = co_await answer();
        co_return base + value;
    }

    Task<void> setFlag(bool& flag)
    {
        flag = true;
        co_return;
    }

    Task<int> thrower()
    {
        throw std::runtime_error("boom");
        co_return 0;
    }

    Task<int> catcher()
    {
        try
        {
            co_return co_await thrower();
        }
        catch (const std::runtime_error&)
        {
            co_return 99;
        }
    }

    Task<int> holdsToken(std::shared_ptr<int> token)
    {
        co_return *token;
    }

    Task<int> one()
    {
        co_return 1;
    }

    Task<int> sumSynchronously(int count)
    {
        int total = 0;
        for (int i = 0; i < count; ++i)
            total += co_await one();
        co_return total;
    }

    // Symmetric transfer needs the compiler to emit a tail call for the
    // resumption. Optimized builds do; unoptimized ones (GCC -O0, MSVC
    // Debug) and sanitizer builds may not, and then every completed
    // co_await costs stack. So the chain is only made long enough to
    // overflow the stack (were symmetric transfer missing) where tail
    // calls are guaranteed.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define DSBOX_TEST_NO_TAIL_CALLS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define DSBOX_TEST_NO_TAIL_CALLS 1
#endif
#endif

#if !defined(DSBOX_TEST_NO_TAIL_CALLS) && (defined(__OPTIMIZE__) || (defined(_MSC_VER) && !defined(_DEBUG)))
    constexpr int kSynchronousChain = 500000;
#else
    constexpr int kSynchronousChain = 2000;
#endif

    void testTaskReturnsValue()
    {
        auto future = spawn(std::make_shared<InlineExecutor>(), addOne(1));
        DSBOX_SCHEDULER_CHECK(isReady(future));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 43);
    }

    void testTaskVoid()
    {
        bool flag = false;
        auto future = spawn(std::make_shared<InlineExecutor>(), setFlag(flag));
        future.get();
        DSBOX_SCHEDULER_CHECK(flag);
    }

    void testTaskIsLazy()
    {
        bool flag = false;
        {
            Task<void> task = setFlag(flag);
            DSBOX_SCHEDULER_CHECK(task);
            DSBOX_SCHEDULER_CHECK(!flag); // created, not started
        }
        DSBOX_SCHEDULER_CHECK(!flag); // and destroying it never starts it
    }

    void testDestroyingUnstartedTaskFreesItsFrame()
    {
        auto token = std::make_shared<int>(7);
        {
            Task<int> task = holdsToken(token);
            DSBOX_SCHEDULER_CHECK_EQ(token.use_count(), 2); // the frame holds a copy
        }
        DSBOX_SCHEDULER_CHECK_EQ(token.use_count(), 1);
    }

    void testTaskMove()
    {
        Task<int> a = answer();
        Task<int> b = std::move(a);
        DSBOX_SCHEDULER_CHECK(!a);
        DSBOX_SCHEDULER_CHECK(b);

        auto future = spawn(std::make_shared<InlineExecutor>(), std::move(b));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 42);
    }

    void testExceptionPropagatesThroughAwait()
    {
        auto future = spawn(std::make_shared<InlineExecutor>(), catcher());
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 99);
    }

    void testExceptionReachesTheFuture()
    {
        auto future = spawn(std::make_shared<InlineExecutor>(), thrower());

        bool threw = false;
        try
        {
            future.get();
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        DSBOX_SCHEDULER_CHECK(threw);
    }

    void testLongSynchronousChainDoesNotOverflowTheStack()
    {
        // Every co_await here completes synchronously; without symmetric
        // transfer each one would add stack frames and the chain would
        // overflow the stack.
        auto future = spawn(std::make_shared<InlineExecutor>(), sumSynchronously(kSynchronousChain));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), kSynchronousChain);
    }

    void testSpawnOnRejectingExecutor()
    {
        auto pool = std::make_shared<ThreadPoolExecutor>(1);
        pool->shutdown();

        auto future = spawn(pool, answer());

        bool threw = false;
        try
        {
            future.get();
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }
        DSBOX_SCHEDULER_CHECK(threw);
    }

    void testSpawnRunsOnTheExecutor()
    {
        LoopExecutor loop;
        auto future = spawn(nonOwning(loop), answer());

        DSBOX_SCHEDULER_CHECK(!isReady(future)); // queued, not run
        loop.poll();
        DSBOX_SCHEDULER_CHECK(isReady(future));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 42);
    }

    // -- sleeping -----------------------------------------------------------

    Task<bool> sleeper(Scheduler& scheduler, Duration delay, int& stage, std::stop_token token = {})
    {
        stage = 1;
        const bool elapsed = co_await scheduler.sleepFor(delay, token);
        stage = 2;
        co_return elapsed;
    }

    void testSleepResumesWhenTimeElapses()
    {
        Manual m;
        int stage = 0;

        auto future = spawn(m.inlineExecutor, sleeper(m.scheduler, 10ms, stage));
        DSBOX_SCHEDULER_CHECK_EQ(stage, 1); // suspended inside the sleep
        DSBOX_SCHEDULER_CHECK(!isReady(future));

        m.advance(9ms);
        DSBOX_SCHEDULER_CHECK_EQ(stage, 1);

        m.advance(1ms);
        DSBOX_SCHEDULER_CHECK_EQ(stage, 2);
        DSBOX_SCHEDULER_CHECK(isReady(future));
        DSBOX_SCHEDULER_CHECK(future.get()); // true: it elapsed
    }

    void testSleepUntilInThePast()
    {
        Manual m;
        m.clock->advance(1s);
        int stage = 0;

        Task<bool> task = [](Scheduler& s, int& stage) -> Task<bool>
        {
            stage = 1;
            const bool ok = co_await s.sleepUntil(s.now() - 1s);
            stage = 2;
            co_return ok;
        }(m.scheduler, stage);

        auto future = spawn(m.inlineExecutor, std::move(task));
        DSBOX_SCHEDULER_CHECK_EQ(stage, 1);

        m.scheduler.runDue();
        DSBOX_SCHEDULER_CHECK_EQ(stage, 2);
        DSBOX_SCHEDULER_CHECK(future.get());
    }

    void testSleepCancelledByStopToken()
    {
        Manual m;
        std::stop_source source;
        int stage = 0;

        auto future = spawn(m.inlineExecutor, sleeper(m.scheduler, 1h, stage, source.get_token()));
        DSBOX_SCHEDULER_CHECK_EQ(stage, 1);
        DSBOX_SCHEDULER_CHECK(m.scheduler.nextDeadline().has_value());

        source.request_stop();

        DSBOX_SCHEDULER_CHECK_EQ(stage, 2); // resumed right away
        DSBOX_SCHEDULER_CHECK(!future.get()); // false: cut short
        DSBOX_SCHEDULER_CHECK(!m.scheduler.nextDeadline().has_value()); // its timer is gone
    }

    void testSleepWithAlreadyStoppedToken()
    {
        Manual m;
        std::stop_source source;
        source.request_stop();
        int stage = 0;

        auto future = spawn(m.inlineExecutor, sleeper(m.scheduler, 1h, stage, source.get_token()));

        DSBOX_SCHEDULER_CHECK_EQ(stage, 2); // never suspended
        DSBOX_SCHEDULER_CHECK(!future.get());
        DSBOX_SCHEDULER_CHECK(!m.scheduler.nextDeadline().has_value());
    }

    void testStopTokenAfterTheSleepEndedIsHarmless()
    {
        Manual m;
        std::stop_source source;
        int stage = 0;

        auto future = spawn(m.inlineExecutor, sleeper(m.scheduler, 10ms, stage, source.get_token()));
        m.advance(10ms);
        DSBOX_SCHEDULER_CHECK(future.get());

        source.request_stop(); // callback was unregistered: nothing to resume twice
        DSBOX_SCHEDULER_CHECK_EQ(stage, 2);
    }

    void testSchedulerStopResumesSleepersAsCancelled()
    {
        Manual m;
        int stageA = 0;
        int stageB = 0;

        auto a = spawn(m.inlineExecutor, sleeper(m.scheduler, 1h, stageA));
        auto b = spawn(m.inlineExecutor, sleeper(m.scheduler, 2h, stageB));
        DSBOX_SCHEDULER_CHECK_EQ(stageA + stageB, 2);

        m.scheduler.stop();

        DSBOX_SCHEDULER_CHECK_EQ(stageA, 2);
        DSBOX_SCHEDULER_CHECK_EQ(stageB, 2);
        DSBOX_SCHEDULER_CHECK(!a.get());
        DSBOX_SCHEDULER_CHECK(!b.get());
    }

    void testSleepOnStoppedSchedulerReturnsFalseImmediately()
    {
        Manual m;
        m.scheduler.stop();
        int stage = 0;

        auto future = spawn(m.inlineExecutor, sleeper(m.scheduler, 10ms, stage));

        DSBOX_SCHEDULER_CHECK_EQ(stage, 2);
        DSBOX_SCHEDULER_CHECK(!future.get());
    }

    Task<bool> sleeperOn(Scheduler& scheduler, std::shared_ptr<IExecutor> executor, int& stage)
    {
        stage = 1;
        const bool elapsed = co_await scheduler.sleepFor(10ms, {}, std::move(executor));
        stage = 2;
        co_return elapsed;
    }

    void testSleepResumesOnTheRequestedExecutor()
    {
        Manual m;
        LoopExecutor loop;
        int stage = 0;

        auto future = spawn(m.inlineExecutor, sleeperOn(m.scheduler, nonOwning(loop), stage));
        m.advance(10ms);

        DSBOX_SCHEDULER_CHECK_EQ(stage, 1); // due, but only the loop may resume it
        loop.poll();
        DSBOX_SCHEDULER_CHECK_EQ(stage, 2);
        DSBOX_SCHEDULER_CHECK(future.get());
    }

    Task<void> repeatedSleeps(Scheduler& scheduler, std::atomic<int>& finished)
    {
        for (int i = 0; i < 5; ++i)
            co_await scheduler.sleepFor(2ms);
        ++finished;
    }

    void testManyConcurrentSleepersOnRealThreads()
    {
        Scheduler scheduler;
        std::atomic<int> finished{ 0 };

        std::vector<std::future<void>> futures;
        for (int i = 0; i < 50; ++i)
            futures.push_back(spawn(scheduler, repeatedSleeps(scheduler, finished)));

        for (auto& future : futures)
            DSBOX_SCHEDULER_CHECK(future.wait_for(10s) == std::future_status::ready);

        DSBOX_SCHEDULER_CHECK_EQ(finished.load(), 50);
    }

    Task<bool> sleepAndReport(Scheduler& scheduler, std::stop_token token)
    {
        co_return co_await scheduler.sleepFor(1h, std::move(token));
    }

    void testStopTokenFromAnotherThread()
    {
        Scheduler scheduler;
        std::stop_source source;

        auto future = spawn(scheduler, sleepAndReport(scheduler, source.get_token()));
        std::this_thread::sleep_for(20ms);
        DSBOX_SCHEDULER_CHECK(!isReady(future));

        std::thread canceller([&] { source.request_stop(); });
        canceller.join();

        DSBOX_SCHEDULER_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_SCHEDULER_CHECK(!future.get());
    }

    void testSchedulerDestructionWithSleepersDoesNotHang()
    {
        std::future<bool> future;

        {
            Scheduler scheduler;
            future = spawn(scheduler, sleepAndReport(scheduler, {}));
            std::this_thread::sleep_for(20ms);
        } // ~Scheduler resumes the sleeper with `false`

        DSBOX_SCHEDULER_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_SCHEDULER_CHECK(!future.get());
    }

    // -- resumeOn -----------------------------------------------------------

    Task<std::thread::id> hopTo(IExecutor& executor)
    {
        co_await resumeOn(executor);
        co_return std::this_thread::get_id();
    }

    void testResumeOnMovesToTheExecutorsThread()
    {
        LoopExecutor loop;
        std::thread pump([&] { loop.run(); });

        auto future = spawn(std::make_shared<InlineExecutor>(), hopTo(loop));
        DSBOX_SCHEDULER_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_SCHEDULER_CHECK(future.get() == pump.get_id());

        loop.stop();
        pump.join();
    }

    Task<bool> hopAndReport(IExecutor& executor)
    {
        co_return co_await resumeOn(executor);
    }

    void testResumeOnRejectedExecutorContinuesInline()
    {
        LoopExecutor loop;
        loop.close();

        auto future = spawn(std::make_shared<InlineExecutor>(), hopAndReport(loop));

        DSBOX_SCHEDULER_CHECK(isReady(future)); // did not get stuck
        DSBOX_SCHEDULER_CHECK(!future.get()); // false: the hop did not happen
    }

    // -- fromCallback -------------------------------------------------------

    Task<int> synchronousCallback()
    {
        co_return co_await fromCallback<int>([](std::function<void(int)> done) { done(5); });
    }

    struct Pending
    {
        std::function<void(int)> done;
    };

    Task<int> asynchronousCallback(Pending& pending)
    {
        co_return co_await fromCallback<int>([&pending](std::function<void(int)> done) { pending.done = std::move(done); });
    }

    Task<int> asynchronousCallbackOn(Pending& pending, std::shared_ptr<IExecutor> executor)
    {
        co_return co_await fromCallback<int>(
            [&pending](std::function<void(int)> done) { pending.done = std::move(done); },
            std::move(executor));
    }

    Task<int> throwingStarter()
    {
        try
        {
            co_return co_await fromCallback<int>([](std::function<void(int)>) { throw std::runtime_error("no"); });
        }
        catch (const std::runtime_error&)
        {
            co_return -1;
        }
    }

    void testCallbackCompletingSynchronously()
    {
        auto future = spawn(std::make_shared<InlineExecutor>(), synchronousCallback());
        DSBOX_SCHEDULER_CHECK(isReady(future));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 5);
    }

    void testCallbackCompletingFromAnotherThread()
    {
        Pending pending;
        auto future = spawn(std::make_shared<InlineExecutor>(), asynchronousCallback(pending));

        DSBOX_SCHEDULER_CHECK(!isReady(future));
        DSBOX_SCHEDULER_CHECK(static_cast<bool>(pending.done));

        std::thread other([&] { pending.done(9); });
        other.join();

        DSBOX_SCHEDULER_CHECK(isReady(future));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 9);
    }

    void testCallbackCalledTwiceIsIgnored()
    {
        Pending pending;
        auto future = spawn(std::make_shared<InlineExecutor>(), asynchronousCallback(pending));

        pending.done(1);
        pending.done(2); // must neither resume again nor overwrite

        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 1);
    }

    void testCallbackResumesOnRequestedExecutor()
    {
        LoopExecutor loop;
        Pending pending;
        auto future = spawn(std::make_shared<InlineExecutor>(), asynchronousCallbackOn(pending, nonOwning(loop)));

        pending.done(3);
        DSBOX_SCHEDULER_CHECK(!isReady(future)); // completion happened, resumption waits for the loop

        loop.poll();
        DSBOX_SCHEDULER_CHECK(isReady(future));
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), 3);
    }

    void testCallbackStarterThrowing()
    {
        auto future = spawn(std::make_shared<InlineExecutor>(), throwingStarter());
        DSBOX_SCHEDULER_CHECK_EQ(future.get(), -1);
    }

    void testCallbackRacingWithSuspension()
    {
        // Completion from a second thread while the awaiting coroutine is
        // still inside await_suspend: whichever side arrives second must
        // do the resuming, never both, never neither.
        for (int round = 0; round < 500; ++round)
        {
            std::atomic<bool> go{ false };
            std::thread completer;

            auto starter = [&](std::function<void(int)> done)
            {
                completer = std::thread(
                    [&go, round, done = std::move(done)]
                    {
                        while (!go.load())
                            std::this_thread::yield();
                        done(round);
                    });
                go = true;
            };

            auto task = [](decltype(starter)& starter) -> Task<int> { co_return co_await fromCallback<int>(starter); }(starter);
            auto future = spawn(std::make_shared<InlineExecutor>(), std::move(task));

            DSBOX_SCHEDULER_CHECK(future.wait_for(5s) == std::future_status::ready);
            completer.join();
            DSBOX_SCHEDULER_CHECK_EQ(future.get(), round);
        }
    }
}

void runCoroutineTests()
{
    using dsbox_scheduler_tests::runTest;

    runTest("Task: returns a value", testTaskReturnsValue);
    runTest("Task: void", testTaskVoid);
    runTest("Task: is lazy", testTaskIsLazy);
    runTest("Task: destroying an unstarted task frees its frame", testDestroyingUnstartedTaskFreesItsFrame);
    runTest("Task: move", testTaskMove);
    runTest("Task: exception propagates through co_await", testExceptionPropagatesThroughAwait);
    runTest("Task: exception reaches the future", testExceptionReachesTheFuture);
    runTest("Task: long synchronous chain (symmetric transfer)", testLongSynchronousChainDoesNotOverflowTheStack);
    runTest("spawn: rejected executor", testSpawnOnRejectingExecutor);
    runTest("spawn: runs on the executor", testSpawnRunsOnTheExecutor);
    runTest("sleep: resumes when time elapses", testSleepResumesWhenTimeElapses);
    runTest("sleep: deadline in the past", testSleepUntilInThePast);
    runTest("sleep: cancelled by stop_token", testSleepCancelledByStopToken);
    runTest("sleep: already stopped token", testSleepWithAlreadyStoppedToken);
    runTest("sleep: late stop_token is harmless", testStopTokenAfterTheSleepEndedIsHarmless);
    runTest("sleep: scheduler stop resumes sleepers as cancelled", testSchedulerStopResumesSleepersAsCancelled);
    runTest("sleep: on a stopped scheduler returns false", testSleepOnStoppedSchedulerReturnsFalseImmediately);
    runTest("sleep: resumes on the requested executor", testSleepResumesOnTheRequestedExecutor);
    runTest("sleep[thread]: many concurrent sleepers", testManyConcurrentSleepersOnRealThreads);
    runTest("sleep[thread]: stop_token from another thread", testStopTokenFromAnotherThread);
    runTest("sleep[thread]: scheduler destruction with sleepers", testSchedulerDestructionWithSleepersDoesNotHang);
    runTest("resumeOn: moves to the executor's thread", testResumeOnMovesToTheExecutorsThread);
    runTest("resumeOn: rejected executor continues inline", testResumeOnRejectedExecutorContinuesInline);
    runTest("fromCallback: synchronous completion", testCallbackCompletingSynchronously);
    runTest("fromCallback: completion from another thread", testCallbackCompletingFromAnotherThread);
    runTest("fromCallback: second call is ignored", testCallbackCalledTwiceIsIgnored);
    runTest("fromCallback: resumes on requested executor", testCallbackResumesOnRequestedExecutor);
    runTest("fromCallback: throwing starter", testCallbackStarterThrowing);
    runTest("fromCallback: completion racing with suspension", testCallbackRacingWithSuspension);
}
