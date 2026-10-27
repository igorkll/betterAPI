#include "test_framework.hpp"

#include <dsbox_scheduler/executor.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace dsbox_scheduler;
using namespace std::chrono_literals;

namespace
{
    void testInlineRunsSynchronously()
    {
        InlineExecutor executor;
        bool ran = false;

        DSBOX_SCHEDULER_CHECK(executor.post([&] { ran = true; }));
        DSBOX_SCHEDULER_CHECK(ran);
    }

    void testNonOwningDoesNotOwn()
    {
        InlineExecutor executor; // on the stack: deleting it would be a crash
        {
            std::shared_ptr<IExecutor> borrowed = nonOwning(executor);
            DSBOX_SCHEDULER_CHECK(borrowed.get() == &executor);
        }
        bool ran = false;
        executor.post([&] { ran = true; });
        DSBOX_SCHEDULER_CHECK(ran);
    }

    void testPoolRunsEverything()
    {
        ThreadPoolExecutor pool(4);
        std::atomic<int> counter{ 0 };

        for (int i = 0; i < 1000; ++i)
            DSBOX_SCHEDULER_CHECK(pool.post([&] { ++counter; }));

        pool.waitIdle();
        DSBOX_SCHEDULER_CHECK_EQ(counter.load(), 1000);
    }

    void testPoolIsLazy()
    {
        // Constructing and destroying an unused pool must be free and safe.
        ThreadPoolExecutor pool(8);
        DSBOX_SCHEDULER_CHECK_EQ(pool.threadCount(), 8u);
        pool.waitIdle(); // nothing queued: returns immediately
    }

    void testPoolSurvivesThrowingCallbacks()
    {
        std::atomic<int> errors{ 0 };
        std::atomic<int> ran{ 0 };

        ThreadPoolExecutor pool(1, [&](std::exception_ptr) { ++errors; });

        pool.post([] { throw std::runtime_error("boom"); });
        pool.post([&] { ++ran; });
        pool.waitIdle();

        DSBOX_SCHEDULER_CHECK_EQ(errors.load(), 1);
        DSBOX_SCHEDULER_CHECK_EQ(ran.load(), 1); // the worker lived on
    }

    void testPoolSwallowsExceptionsWithoutHandler()
    {
        ThreadPoolExecutor pool(1);
        std::atomic<int> ran{ 0 };

        pool.post([] { throw 42; });
        pool.post([&] { ++ran; });
        pool.waitIdle();

        DSBOX_SCHEDULER_CHECK_EQ(ran.load(), 1);
    }

    void testPoolShutdownDrainsThenRejects()
    {
        ThreadPoolExecutor pool(1);
        std::atomic<int> counter{ 0 };

        for (int i = 0; i < 50; ++i)
            pool.post([&] { std::this_thread::sleep_for(1ms); ++counter; });

        pool.shutdown();

        DSBOX_SCHEDULER_CHECK_EQ(counter.load(), 50); // queued work was not lost
        DSBOX_SCHEDULER_CHECK(!pool.post([&] { ++counter; }));
        DSBOX_SCHEDULER_CHECK_EQ(counter.load(), 50);

        pool.shutdown(); // idempotent
    }

    void testPoolShutdownFromOwnCallback()
    {
        auto pool = std::make_shared<ThreadPoolExecutor>(2);
        auto slot = std::make_shared<std::shared_ptr<ThreadPoolExecutor>>(pool);
        std::atomic<bool> released{ false };
        std::atomic<bool> done{ false };

        // The callback holds the last reference to the pool and drops it,
        // so the pool is destroyed (and shut down) from one of its own
        // workers. `released` keeps it from doing so before the test
        // thread has let go of its own references.
        pool->post(
            [slot, &released, &done]
            {
                while (!released.load())
                    std::this_thread::yield();

                slot->reset();
                done = true;
            });

        slot.reset();
        pool.reset();
        released = true;

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return done.load(); }));
    }

    void testLoopRunsOnlyWhenPumped()
    {
        LoopExecutor loop;
        std::vector<int> order;

        loop.post([&] { order.push_back(1); });
        loop.post([&] { order.push_back(2); });

        DSBOX_SCHEDULER_CHECK(order.empty());
        DSBOX_SCHEDULER_CHECK_EQ(loop.pending(), 2u);

        DSBOX_SCHEDULER_CHECK(loop.runOne());
        DSBOX_SCHEDULER_CHECK_EQ(order.size(), 1u);

        DSBOX_SCHEDULER_CHECK_EQ(loop.poll(), 1u);
        DSBOX_SCHEDULER_CHECK((order == std::vector<int>{ 1, 2 }));
        DSBOX_SCHEDULER_CHECK(!loop.runOne());
    }

    void testLoopPollRunsCallbacksQueuedByCallbacks()
    {
        LoopExecutor loop;
        int count = 0;

        loop.post(
            [&]
            {
                ++count;
                loop.post([&] { ++count; });
            });

        DSBOX_SCHEDULER_CHECK_EQ(loop.poll(), 2u);
        DSBOX_SCHEDULER_CHECK_EQ(count, 2);
    }

    void testLoopRunsOnPumpingThread()
    {
        LoopExecutor loop;
        std::thread::id ranOn;

        std::thread poster([&] { loop.post([&] { ranOn = std::this_thread::get_id(); }); });
        poster.join();

        loop.poll();
        DSBOX_SCHEDULER_CHECK(ranOn == std::this_thread::get_id());
    }

    void testLoopRunBlocksUntilStop()
    {
        LoopExecutor loop;
        std::atomic<int> count{ 0 };
        std::atomic<bool> finished{ false };

        std::thread pump(
            [&]
            {
                loop.run();
                finished = true;
            });

        for (int i = 0; i < 10; ++i)
            loop.post([&] { ++count; });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return count.load() == 10; }));
        DSBOX_SCHEDULER_CHECK(!finished.load());

        loop.stop();
        pump.join();
        DSBOX_SCHEDULER_CHECK(finished.load());

        loop.restart();
        loop.post([&] { ++count; });
        DSBOX_SCHEDULER_CHECK_EQ(loop.poll(), 1u);
    }

    void testLoopRunForTimesOut()
    {
        LoopExecutor loop;

        const auto start = std::chrono::steady_clock::now();
        const std::size_t ran = loop.runFor(30ms);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        DSBOX_SCHEDULER_CHECK_EQ(ran, 0u);
        DSBOX_SCHEDULER_CHECK(elapsed >= 25ms);
    }

    void testLoopRunForProcessesArrivals()
    {
        LoopExecutor loop;
        std::atomic<int> count{ 0 };

        std::thread poster(
            [&]
            {
                std::this_thread::sleep_for(5ms);
                loop.post([&] { ++count; });
            });

        loop.runFor(200ms);
        poster.join();

        DSBOX_SCHEDULER_CHECK_EQ(count.load(), 1);
    }

    void testLoopCloseRejectsButKeepsQueued()
    {
        LoopExecutor loop;
        int count = 0;

        loop.post([&] { ++count; });
        loop.close();

        DSBOX_SCHEDULER_CHECK(!loop.post([&] { ++count; }));
        DSBOX_SCHEDULER_CHECK_EQ(loop.poll(), 1u);
        DSBOX_SCHEDULER_CHECK_EQ(count, 1);
    }

    void testLoopSurvivesThrowingCallbacks()
    {
        int errors = 0;
        LoopExecutor loop([&](std::exception_ptr) { ++errors; });

        loop.post([] { throw std::runtime_error("boom"); });
        loop.post([] {});

        DSBOX_SCHEDULER_CHECK_EQ(loop.poll(), 2u);
        DSBOX_SCHEDULER_CHECK_EQ(errors, 1);
    }

    void testSerialNeverOverlapsAndKeepsOrder()
    {
        auto pool = std::make_shared<ThreadPoolExecutor>(4);
        SerialExecutor serial(pool);

        constexpr int kPosters = 4;
        constexpr int kPerPoster = 500;

        std::atomic<int> concurrent{ 0 };
        std::atomic<int> maxConcurrent{ 0 };
        std::atomic<int> total{ 0 };

        std::mutex logMutex; // only guards `last`, to detect reordering per poster
        std::vector<int> last(kPosters, -1);
        std::atomic<bool> outOfOrder{ false };

        std::vector<std::thread> posters;
        for (int p = 0; p < kPosters; ++p)
        {
            posters.emplace_back(
                [&, p]
                {
                    for (int i = 0; i < kPerPoster; ++i)
                    {
                        serial.post(
                            [&, p, i]
                            {
                                const int now = ++concurrent;
                                int seen = maxConcurrent.load();
                                while (now > seen && !maxConcurrent.compare_exchange_weak(seen, now))
                                {
                                }

                                {
                                    std::lock_guard lock(logMutex);
                                    if (last[p] != i - 1)
                                        outOfOrder = true;
                                    last[p] = i;
                                }

                                ++total;
                                --concurrent;
                            });
                    }
                });
        }

        for (auto& poster : posters)
            poster.join();

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return total.load() == kPosters * kPerPoster; }));
        DSBOX_SCHEDULER_CHECK_EQ(maxConcurrent.load(), 1);
        DSBOX_SCHEDULER_CHECK(!outOfOrder.load());
    }

    void testSerialSurvivesThrowingCallback()
    {
        auto pool = std::make_shared<ThreadPoolExecutor>(2);
        std::atomic<int> errors{ 0 };
        std::atomic<int> ran{ 0 };
        SerialExecutor serial(pool, [&](std::exception_ptr) { ++errors; });

        serial.post([] { throw std::runtime_error("boom"); });
        serial.post([&] { ++ran; });

        DSBOX_SCHEDULER_CHECK(dsbox_scheduler_tests::waitFor([&] { return ran.load() == 1; }));
        DSBOX_SCHEDULER_CHECK_EQ(errors.load(), 1);
    }

    void testSerialRejectsWhenBaseIsShutDown()
    {
        auto pool = std::make_shared<ThreadPoolExecutor>(1);
        SerialExecutor serial(pool);

        pool->shutdown();
        DSBOX_SCHEDULER_CHECK(!serial.post([] {}));
    }
}

void runExecutorTests()
{
    using dsbox_scheduler_tests::runTest;

    runTest("Inline: runs synchronously", testInlineRunsSynchronously);
    runTest("nonOwning: does not take ownership", testNonOwningDoesNotOwn);
    runTest("ThreadPool: runs everything", testPoolRunsEverything);
    runTest("ThreadPool: unused pool is harmless", testPoolIsLazy);
    runTest("ThreadPool: survives throwing callbacks", testPoolSurvivesThrowingCallbacks);
    runTest("ThreadPool: swallows exceptions without a handler", testPoolSwallowsExceptionsWithoutHandler);
    runTest("ThreadPool: shutdown drains then rejects", testPoolShutdownDrainsThenRejects);
    runTest("ThreadPool: destroyed from its own callback", testPoolShutdownFromOwnCallback);
    runTest("Loop: runs only when pumped", testLoopRunsOnlyWhenPumped);
    runTest("Loop: poll runs callbacks queued by callbacks", testLoopPollRunsCallbacksQueuedByCallbacks);
    runTest("Loop: runs on the pumping thread", testLoopRunsOnPumpingThread);
    runTest("Loop: run() blocks until stop()", testLoopRunBlocksUntilStop);
    runTest("Loop: runFor() times out", testLoopRunForTimesOut);
    runTest("Loop: runFor() processes arrivals", testLoopRunForProcessesArrivals);
    runTest("Loop: close() rejects but keeps queued", testLoopCloseRejectsButKeepsQueued);
    runTest("Loop: survives throwing callbacks", testLoopSurvivesThrowingCallbacks);
    runTest("Serial: never overlaps, keeps order", testSerialNeverOverlapsAndKeepsOrder);
    runTest("Serial: survives throwing callbacks", testSerialSurvivesThrowingCallback);
    runTest("Serial: rejects when base is shut down", testSerialRejectsWhenBaseIsShutDown);
}
