#include "test_framework.hpp"

#include <dsbox_io/io_module.hpp>

#include <dsbox/context.hpp>

#include <dsbox_ipc/frame_codec.hpp>
#include <dsbox_ipc/ipc_channel.hpp>

#include <dsbox_scheduler/clock.hpp>
#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Captures delivered messages instead of letting them reach a real
    // stream, so tests can assert on exactly what was (and wasn't)
    // delivered.
    struct Capture
    {
        std::mutex mutex;
        std::vector<std::string> messages;

        void add(std::string text)
        {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back(std::move(text));
        }
    };

    // Redirects a std::ostream (std::cout or std::cerr) to an in-memory
    // buffer for the duration of its lifetime, for verifying the direct
    // (no override registered, or no CommunicationManager reference
    // available yet) write path.
    class StreamCapture
    {
    public:
        explicit StreamCapture(std::ostream& stream)
            : m_stream(stream)
            , m_original(stream.rdbuf(m_buffer.rdbuf()))
        {
        }

        ~StreamCapture() { m_stream.rdbuf(m_original); }

        StreamCapture(const StreamCapture&) = delete;
        StreamCapture& operator=(const StreamCapture&) = delete;

        std::string str() const { return m_buffer.str(); }

    private:
        std::ostream& m_stream;
        std::ostringstream m_buffer;
        std::streambuf* m_original;
    };

    /*
     * stdout (write) -- asynchronous, queued.
     */

    void testWritesFromMultipleThreadsAllDelivered()
    {
        dsbox::Context context;
        Capture capture;

        context.comm().registerHandler<void(std::string)>(
            "io.stdout.write",
            [&](std::string text) { capture.add(std::move(text)); });

        dsbox_io::IOModule io;
        io.onInit(context);

        constexpr int kThreads = 8;
        constexpr int kPerThread = 200;

        std::vector<std::thread> writers;
        for (int t = 0; t < kThreads; ++t)
        {
            writers.emplace_back([&, t]
            {
                for (int i = 0; i < kPerThread; ++i)
                    io.write("t" + std::to_string(t) + "-" + std::to_string(i));
            });
        }

        for (auto& w : writers)
            w.join();

        // Blocks until every queued message has been delivered.
        io.onShutdown(context);

        DSBOX_IO_CHECK_EQ(
            capture.messages.size(), static_cast<std::size_t>(kThreads * kPerThread));
    }

    void testPerThreadOrderIsPreserved()
    {
        dsbox::Context context;
        Capture capture;

        context.comm().registerHandler<void(std::string)>(
            "io.stdout.write",
            [&](std::string text) { capture.add(std::move(text)); });

        dsbox_io::IOModule io;
        io.onInit(context);

        for (int i = 0; i < 500; ++i)
            io.write(std::to_string(i));

        io.onShutdown(context);

        DSBOX_IO_CHECK_EQ(capture.messages.size(), std::size_t{ 500 });

        bool inOrder = true;
        for (std::size_t i = 0; i < capture.messages.size(); ++i)
            if (capture.messages.at(i) != std::to_string(i))
                inOrder = false;

        DSBOX_IO_CHECK(inOrder);
    }

    void testWriteBeforeInitWritesDirectlyToStdout()
    {
        dsbox::Context context;
        dsbox_io::IOModule io;

        StreamCapture capture(std::cout);
        io.write("early");
        std::string output = capture.str();

        DSBOX_IO_CHECK(output.find("early") != std::string::npos);
    }

    void testWriteAfterShutdownFallsBackToSynchronous()
    {
        dsbox::Context context;
        Capture capture;

        context.comm().registerHandler<void(std::string)>(
            "io.stdout.write",
            [&](std::string text) { capture.add(std::move(text)); });

        dsbox_io::IOModule io;
        io.onInit(context);
        io.onShutdown(context);

        io.write("late");

        DSBOX_IO_CHECK_EQ(capture.messages.size(), std::size_t{ 1 });
        if (capture.messages.size() == 1)
            DSBOX_IO_CHECK_EQ(capture.messages.at(0), std::string("late"));
    }

    void testThrowingOverrideDoesNotStopSubsequentDelivery()
    {
        dsbox::Context context;
        Capture capture;
        std::atomic<int> calls{ 0 };

        context.comm().registerHandler<void(std::string)>(
            "io.stdout.write",
            [&](std::string text)
            {
                int n = calls++;

                if (n == 1)
                    throw std::runtime_error("boom");

                capture.add(std::move(text));
            });

        dsbox_io::IOModule io;
        io.onInit(context);

        io.write("a");
        io.write("b"); // the handler throws for this one
        io.write("c");

        io.onShutdown(context);

        DSBOX_IO_CHECK_EQ(calls.load(), 3);
        DSBOX_IO_CHECK_EQ(capture.messages.size(), std::size_t{ 2 });
    }

    void testShutdownIsIdempotent()
    {
        dsbox::Context context;

        dsbox_io::IOModule io;
        io.onInit(context);
        io.write("x");
        io.onShutdown(context);
        io.onShutdown(context); // must not crash or double-join
    }

    void testDestructorStopsRunningWorkerSafely()
    {
        dsbox::Context context;
        Capture capture;

        context.comm().registerHandler<void(std::string)>(
            "io.stdout.write",
            [&](std::string text) { capture.add(std::move(text)); });

        {
            dsbox_io::IOModule io;
            io.onInit(context);
            io.write("during-destruction");
        }

        DSBOX_IO_CHECK_EQ(capture.messages.size(), std::size_t{ 1 });
    }

    /*
     * stderr (writeError) -- synchronous, not queued.
     */

    void testWriteErrorRoutesThroughOverride()
    {
        dsbox::Context context;
        Capture capture;

        context.comm().registerHandler<void(std::string)>(
            "io.stderr.write",
            [&](std::string text) { capture.add(std::move(text)); });

        dsbox_io::IOModule io;
        io.onInit(context);

        io.writeError("oops");

        DSBOX_IO_CHECK_EQ(capture.messages.size(), std::size_t{ 1 });
        if (capture.messages.size() == 1)
            DSBOX_IO_CHECK_EQ(capture.messages.at(0), std::string("oops"));

        io.onShutdown(context);
    }

    void testWriteErrorIsSynchronousNotQueued()
    {
        // No worker thread involved for stderr, unlike write() -- the
        // override must already have run by the time writeError()
        // returns, with no wait/synchronization needed to observe it.
        dsbox::Context context;
        bool delivered = false;

        context.comm().registerHandler<void(std::string)>(
            "io.stderr.write",
            [&](std::string) { delivered = true; });

        dsbox_io::IOModule io;
        io.onInit(context);

        io.writeError("x");

        DSBOX_IO_CHECK(delivered);

        io.onShutdown(context);
    }

    void testWriteErrorBeforeInitWritesDirectlyToStderr()
    {
        dsbox::Context context;
        dsbox_io::IOModule io;

        StreamCapture capture(std::cerr);
        io.writeError("early-err");
        std::string output = capture.str();

        DSBOX_IO_CHECK(output.find("early-err") != std::string::npos);
    }

    void testWriteErrorThrowingOverrideDoesNotPropagate()
    {
        dsbox::Context context;

        context.comm().registerHandler<void(std::string)>(
            "io.stderr.write",
            [&](std::string) { throw std::runtime_error("boom"); });

        dsbox_io::IOModule io;
        io.onInit(context);

        io.writeError("x"); // must not throw out to the caller

        io.onShutdown(context);
    }

    /*
     * stdin (enableStdin) -- opt-in, detached reader thread.
     */

    void testEnableStdinPublishesLinesAsEvents()
    {
        dsbox::Context context;

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::string> received;

        context.comm().registerEvent<std::string>(
            "io.stdin.line",
            [&](std::string line)
            {
                std::lock_guard<std::mutex> lock(mutex);
                received.push_back(std::move(line));
                cv.notify_one();
            });

        dsbox_io::IOModule io;
        io.onInit(context);

        // Static and never restored: the reader thread is detached and
        // cannot be joined, so there is no point at which it's safe to
        // know it has stopped touching this buffer or std::cin's rdbuf.
        // Giving both static duration for the rest of the process avoids
        // ever handing the thread a dangling reference, mirroring the
        // exact constraint enableStdin() documents for production use.
        static std::istringstream fakeStdin("first\nsecond\nthird\n");
        std::cin.rdbuf(fakeStdin.rdbuf());

        io.enableStdin();

        std::unique_lock<std::mutex> lock(mutex);
        bool ok = cv.wait_for(
            lock, std::chrono::seconds(5), [&] { return received.size() >= 3; });

        DSBOX_IO_CHECK(ok);
        DSBOX_IO_CHECK_EQ(received.size(), std::size_t{ 3 });

        if (received.size() == 3)
        {
            DSBOX_IO_CHECK_EQ(received.at(0), std::string("first"));
            DSBOX_IO_CHECK_EQ(received.at(1), std::string("second"));
            DSBOX_IO_CHECK_EQ(received.at(2), std::string("third"));
        }

        lock.unlock();

        io.onShutdown(context); // stops the stdout worker only
    }

    void testEnableStdinIsIdempotent()
    {
        dsbox::Context context;

        std::mutex mutex;
        std::condition_variable cv;
        std::vector<std::string> received;

        context.comm().registerEvent<std::string>(
            "io.stdin.line",
            [&](std::string line)
            {
                std::lock_guard<std::mutex> lock(mutex);
                received.push_back(std::move(line));
                cv.notify_one();
            });

        dsbox_io::IOModule io;
        io.onInit(context);

        static std::istringstream fakeStdin("only\n");
        std::cin.rdbuf(fakeStdin.rdbuf());

        io.enableStdin();
        io.enableStdin(); // must be a no-op, not start a second reader

        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait_for(lock, std::chrono::seconds(5), [&] { return !received.empty(); });
        }

        // Give a duplicate reader thread (if the no-op guard were
        // broken) a moment to also have delivered its line by now.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::lock_guard<std::mutex> lock(mutex);
        DSBOX_IO_CHECK_EQ(received.size(), std::size_t{ 1 });

        io.onShutdown(context);
    }

    void testEnableStdinBeforeInitIsNoop()
    {
        dsbox::Context context;
        dsbox_io::IOModule io;

        // Deliberately not calling onInit() -- there's no Context to
        // emit through yet, this must not crash or start a thread.
        io.enableStdin();
    }

    /*
     * IPC request timeouts -- run on the Context's shared scheduler.
     */

    // Stands in for std::cin's buffer: never produces a byte, and blocks
    // the reading thread inside underflow() for good. connectAsChild()'s
    // reader thread is detached and treats EOF as "the link is gone"
    // (it closes the channel), so a stdin that ended right away would
    // fail the pending request for the wrong reason.
    class SilentStdin final : public std::streambuf
    {
    public:
        // True once a reader has arrived in underflow(), i.e. the
        // detached reader thread is up and parked.
        bool waitForReader(std::chrono::milliseconds timeout)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            return m_cv.wait_for(lock, timeout, [this] { return m_readerArrived; });
        }

    protected:
        int_type underflow() override
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_readerArrived = true;
            m_cv.notify_all();
            m_cv.wait(lock, [] { return false; }); // for the rest of the process
            return traits_type::eof();
        }

    private:
        std::mutex m_mutex;
        std::condition_variable m_cv;
        bool m_readerArrived = false;
    };

    void testChildChannelTimeoutsRunOnSharedScheduler()
    {
        dsbox::Context context;

        // A scheduler nobody drives but this test: no timer thread, time
        // only moves when the test says so. A channel that ignored it
        // and used a private (real-time) scheduler would never time out
        // a one-hour request here.
        auto clock = std::make_shared<dsbox_scheduler::ManualClock>();

        dsbox_scheduler::SchedulerOptions schedulerOptions;
        schedulerOptions.clock = clock;
        schedulerOptions.useTimerThread = false;
        schedulerOptions.defaultExecutor = std::make_shared<dsbox_scheduler::InlineExecutor>();

        auto scheduler = std::make_shared<dsbox_scheduler::Scheduler>(std::move(schedulerOptions));
        context.provide(scheduler);

        dsbox_io::IOModule io;
        io.onInit(context);

        // stdout is the wire once connectAsChild() has run: keep the
        // frame it writes out of the test log.
        StreamCapture wire(std::cout);

        // Never destroyed, never released: its reader thread stays
        // parked in it until the process exits (see SilentStdin).
        static SilentStdin* const silentStdin = new SilentStdin;
        std::cin.rdbuf(silentStdin);

        auto channel = io.connectAsChild();

        // The detached reader reads module state when it starts; do not
        // let `io` die before it has gone past that point.
        DSBOX_IO_CHECK(silentStdin->waitForReader(std::chrono::seconds(5)));

        auto reply = channel->request(
            "ping",
            std::vector<std::byte>{ std::byte{ 'x' } },
            std::chrono::hours(1));

        DSBOX_IO_CHECK(reply.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

        clock->advance(std::chrono::hours(1));
        DSBOX_IO_CHECK_EQ(scheduler->runDue(), std::size_t{ 1 });

        const bool ready = reply.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        DSBOX_IO_CHECK(ready);

        // get() only once ready: if the timeout did not fire, it would
        // block for the whole hour instead of failing the test.
        bool failed = false;
        if (ready)
        {
            try
            {
                reply.get();
            }
            catch (const std::runtime_error&)
            {
                failed = true;
            }
        }

        DSBOX_IO_CHECK(failed);

        io.onShutdown(context);
    }
}

void runIOModuleTests()
{
    using namespace dsbox_io_tests;

    runTest("WritesFromMultipleThreadsAllDelivered", testWritesFromMultipleThreadsAllDelivered);
    runTest("PerThreadOrderIsPreserved", testPerThreadOrderIsPreserved);
    runTest("WriteBeforeInitWritesDirectlyToStdout", testWriteBeforeInitWritesDirectlyToStdout);
    runTest("WriteAfterShutdownFallsBackToSynchronous", testWriteAfterShutdownFallsBackToSynchronous);
    runTest(
        "ThrowingOverrideDoesNotStopSubsequentDelivery",
        testThrowingOverrideDoesNotStopSubsequentDelivery);
    runTest("ShutdownIsIdempotent", testShutdownIsIdempotent);
    runTest("DestructorStopsRunningWorkerSafely", testDestructorStopsRunningWorkerSafely);

    runTest("WriteErrorRoutesThroughOverride", testWriteErrorRoutesThroughOverride);
    runTest("WriteErrorIsSynchronousNotQueued", testWriteErrorIsSynchronousNotQueued);
    runTest(
        "WriteErrorBeforeInitWritesDirectlyToStderr",
        testWriteErrorBeforeInitWritesDirectlyToStderr);
    runTest(
        "WriteErrorThrowingOverrideDoesNotPropagate",
        testWriteErrorThrowingOverrideDoesNotPropagate);

    runTest("EnableStdinPublishesLinesAsEvents", testEnableStdinPublishesLinesAsEvents);
    runTest("EnableStdinIsIdempotent", testEnableStdinIsIdempotent);
    runTest("EnableStdinBeforeInitIsNoop", testEnableStdinBeforeInitIsNoop);

    // Last on purpose: leaves a reader thread parked on std::cin for the
    // rest of the process, which must not overlap any other stdin test.
    runTest(
        "ChildChannelTimeoutsRunOnSharedScheduler",
        testChildChannelTimeoutsRunOnSharedScheduler);
}
