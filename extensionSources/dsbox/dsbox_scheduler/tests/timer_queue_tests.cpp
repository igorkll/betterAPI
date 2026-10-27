#include "test_framework.hpp"

#include <dsbox_scheduler/timer_queue.hpp>

#include <chrono>
#include <vector>

using namespace dsbox_scheduler;
using namespace std::chrono_literals;

namespace
{
    const TimePoint t0{};

    TimerEntry entry(TimePoint deadline, std::vector<int>& log, int tag)
    {
        TimerEntry e;
        e.deadline = deadline;
        e.task = [&log, tag] { log.push_back(tag); };
        return e;
    }

    void runAll(std::vector<TimerEntry> entries)
    {
        for (auto& e : entries)
            e.task();
    }

    void testPopsInDeadlineOrder()
    {
        std::vector<int> log;
        TimerQueue queue;
        queue.add(entry(t0 + 30ms, log, 3));
        queue.add(entry(t0 + 10ms, log, 1));
        queue.add(entry(t0 + 20ms, log, 2));

        runAll(queue.popDue(t0 + 1s));

        DSBOX_SCHEDULER_CHECK_EQ(log.size(), 3u);
        DSBOX_SCHEDULER_CHECK((log == std::vector<int>{ 1, 2, 3 }));
        DSBOX_SCHEDULER_CHECK(queue.empty());
    }

    void testEqualDeadlinesAreFifo()
    {
        std::vector<int> log;
        TimerQueue queue;
        for (int i = 1; i <= 5; ++i)
            queue.add(entry(t0 + 10ms, log, i));

        runAll(queue.popDue(t0 + 10ms));

        DSBOX_SCHEDULER_CHECK((log == std::vector<int>{ 1, 2, 3, 4, 5 }));
    }

    void testPopDueTakesOnlyDueEntries()
    {
        std::vector<int> log;
        TimerQueue queue;
        queue.add(entry(t0 + 10ms, log, 1));
        queue.add(entry(t0 + 20ms, log, 2));

        DSBOX_SCHEDULER_CHECK_EQ(queue.popDue(t0 + 9ms).size(), 0u);
        DSBOX_SCHEDULER_CHECK_EQ(queue.popDue(t0 + 10ms).size(), 1u); // boundary is inclusive
        DSBOX_SCHEDULER_CHECK_EQ(queue.size(), 1u);
        DSBOX_SCHEDULER_CHECK_EQ(queue.popDue(t0 + 20ms).size(), 1u);
        DSBOX_SCHEDULER_CHECK(queue.empty());
    }

    void testNextDeadline()
    {
        std::vector<int> log;
        TimerQueue queue;

        DSBOX_SCHEDULER_CHECK(!queue.nextDeadline().has_value());

        queue.add(entry(t0 + 20ms, log, 2));
        DSBOX_SCHEDULER_CHECK(queue.nextDeadline() == t0 + 20ms);

        queue.add(entry(t0 + 5ms, log, 1));
        DSBOX_SCHEDULER_CHECK(queue.nextDeadline() == t0 + 5ms);
    }

    void testCancel()
    {
        std::vector<int> log;
        TimerQueue queue;
        const TimerId a = queue.add(entry(t0 + 10ms, log, 1));
        const TimerId b = queue.add(entry(t0 + 20ms, log, 2));

        DSBOX_SCHEDULER_CHECK(queue.cancel(a));
        DSBOX_SCHEDULER_CHECK(!queue.cancel(a)); // already gone
        DSBOX_SCHEDULER_CHECK(!queue.cancel(9999));
        DSBOX_SCHEDULER_CHECK(queue.nextDeadline() == t0 + 20ms);

        auto due = queue.popDue(t0 + 1s);
        DSBOX_SCHEDULER_CHECK_EQ(due.size(), 1u);
        DSBOX_SCHEDULER_CHECK_EQ(due[0].id, b);
        DSBOX_SCHEDULER_CHECK(!queue.cancel(b)); // popped entries are no longer cancellable
    }

    void testIdsAreUnique()
    {
        std::vector<int> log;
        TimerQueue queue;
        const TimerId a = queue.add(entry(t0, log, 1));
        const TimerId b = queue.add(entry(t0, log, 2));

        DSBOX_SCHEDULER_CHECK(a != b);
        DSBOX_SCHEDULER_CHECK(a != 0 && b != 0);
    }

    void testDrainReturnsEverythingInOrder()
    {
        std::vector<int> log;
        TimerQueue queue;
        queue.add(entry(t0 + 20ms, log, 2));
        queue.add(entry(t0 + 10ms, log, 1));

        runAll(queue.drain());

        DSBOX_SCHEDULER_CHECK((log == std::vector<int>{ 1, 2 }));
        DSBOX_SCHEDULER_CHECK(queue.empty());
        DSBOX_SCHEDULER_CHECK(!queue.nextDeadline().has_value());
    }
}

void runTimerQueueTests()
{
    using dsbox_scheduler_tests::runTest;

    runTest("TimerQueue: pops in deadline order", testPopsInDeadlineOrder);
    runTest("TimerQueue: equal deadlines are FIFO", testEqualDeadlinesAreFifo);
    runTest("TimerQueue: popDue takes only due entries", testPopDueTakesOnlyDueEntries);
    runTest("TimerQueue: nextDeadline", testNextDeadline);
    runTest("TimerQueue: cancel", testCancel);
    runTest("TimerQueue: ids are unique", testIdsAreUnique);
    runTest("TimerQueue: drain", testDrainReturnsEverythingInOrder);
}
