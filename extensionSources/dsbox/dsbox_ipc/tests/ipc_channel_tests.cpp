#include "test_framework.hpp"

#include <dsbox_ipc/ipc_channel.hpp>
#include <dsbox_ipc/frame_codec.hpp>

#include <dsbox_scheduler/clock.hpp>
#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace dsbox_ipc_tests;
using namespace dsbox_ipc;
using namespace std::chrono_literals;

namespace
{
    std::vector<std::byte> toBytes(const std::string& text)
    {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    std::string toString(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // Wires two IpcChannels directly to each other in-memory (no real
    // transport, no threads) -- every write on one side is fed straight
    // into the other's onBytesReceived synchronously. This is enough to
    // exercise the whole request/response/notify/handler contract
    // without depending on dsbox_process or real pipes.
    struct Loopback
    {
        std::shared_ptr<IpcChannel> a;
        std::shared_ptr<IpcChannel> b;

        // Raw pointers as *members* (not locals!) so the WriteFn lambdas
        // below keep referring to valid storage after the constructor
        // returns -- capturing a reference to a constructor-local would
        // dangle the moment construction finished.
        IpcChannel* aPtr = nullptr;
        IpcChannel* bPtr = nullptr;

        Loopback()
        {
            a = std::make_shared<IpcChannel>(std::make_unique<LengthPrefixedFrameCodec>(),
                [this](std::span<const std::byte> bytes) { bPtr->onBytesReceived(bytes); });
            b = std::make_shared<IpcChannel>(std::make_unique<LengthPrefixedFrameCodec>(),
                [this](std::span<const std::byte> bytes) { aPtr->onBytesReceived(bytes); });

            bPtr = b.get();
            aPtr = a.get();
        }
    };

    void testRequestResponseRoundTrip()
    {
        Loopback lb;

        lb.b->registerHandler("echo", [](std::vector<std::byte> payload)
        {
            return payload; // echo it back
        });

        std::future<std::vector<std::byte>> future = lb.a->request("echo", toBytes("ping"));

        DSBOX_IPC_CHECK(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        DSBOX_IPC_CHECK_EQ(toString(future.get()), "ping");
    }

    void testNotifyDeliveredWithoutResponse()
    {
        Loopback lb;

        std::string received;
        lb.b->registerHandler("log", [&received](std::vector<std::byte> payload)
        {
            received = toString(payload);
            return std::vector<std::byte>{};
        });

        lb.a->notify("log", toBytes("hello"));

        DSBOX_IPC_CHECK_EQ(received, "hello");
    }

    void testRequestToUnknownTargetFails()
    {
        Loopback lb;

        std::future<std::vector<std::byte>> future = lb.a->request("nobody.home", toBytes("x"));

        bool threw = false;
        try { future.get(); }
        catch (const std::exception&) { threw = true; }

        DSBOX_IPC_CHECK(threw);
    }

    void testDuplicateHandlerRegistrationThrows()
    {
        Loopback lb;
        lb.b->registerHandler("dup", [](std::vector<std::byte> p) { return p; });

        bool threw = false;
        try { lb.b->registerHandler("dup", [](std::vector<std::byte> p) { return p; }); }
        catch (const std::runtime_error&) { threw = true; }

        DSBOX_IPC_CHECK(threw);
    }

    void testCloseFailsPendingRequests()
    {
        Loopback lb;
        // No handler registered for "never.answers" -- but instead of
        // letting the unknown-target Error come back, we close the
        // channel first to exercise the "pending request cancelled by
        // close()" path specifically (requestAsync bypassing send()
        // would be needed to truly race this in a single thread, so
        // here we just verify close() after the fact still resolves any
        // requests that are, for whatever reason, still pending).
        std::atomic<bool> called{ false };
        std::atomic<bool> ok{ true };
        lb.a->requestAsync("whatever", {}, [&](bool wasOk, std::vector<std::byte>)
        {
            called = true;
            ok = wasOk;
        });

        lb.a->close();

        DSBOX_IPC_CHECK(called.load());
    }

    // -- request timeouts ----------------------------------------------------

    // A scheduler driven by hand: manual clock, no timer thread, callbacks
    // run inline inside runDue(). Timeout tests using it never sleep.
    struct ManualScheduler
    {
        std::shared_ptr<dsbox_scheduler::ManualClock> clock = std::make_shared<dsbox_scheduler::ManualClock>();
        std::shared_ptr<dsbox_scheduler::Scheduler> scheduler;

        ManualScheduler()
        {
            dsbox_scheduler::SchedulerOptions options;
            options.clock = clock;
            options.defaultExecutor = std::make_shared<dsbox_scheduler::InlineExecutor>();
            options.useTimerThread = false;
            scheduler = std::make_shared<dsbox_scheduler::Scheduler>(std::move(options));
        }

        void advance(std::chrono::milliseconds by)
        {
            clock->advance(by);
            scheduler->runDue();
        }
    };

    // A channel whose peer never answers: everything written to it just
    // vanishes.
    std::shared_ptr<IpcChannel> silentChannel(IpcChannelOptions options = {})
    {
        return std::make_shared<IpcChannel>(
            std::make_unique<LengthPrefixedFrameCodec>(),
            [](std::span<const std::byte>) {},
            std::move(options));
    }

    // Polls `condition` until it holds or `timeout` passes.
    template<typename Predicate>
    bool waitUntil(Predicate condition, std::chrono::milliseconds timeout = 5000ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (!condition())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;

            std::this_thread::sleep_for(1ms);
        }

        return true;
    }

    void testRequestTimesOutOnInjectedScheduler()
    {
        ManualScheduler ms;
        IpcChannelOptions options;
        options.scheduler = ms.scheduler;
        auto channel = silentChannel(options);

        std::atomic<int> calls{ 0 };
        std::atomic<bool> ok{ true };
        channel->requestAsync("x", {}, [&](bool wasOk, std::vector<std::byte>)
        {
            ++calls;
            ok = wasOk;
        }, 100ms);

        ms.advance(99ms);
        DSBOX_IPC_CHECK_EQ(calls.load(), 0);

        ms.advance(1ms);
        DSBOX_IPC_CHECK_EQ(calls.load(), 1);
        DSBOX_IPC_CHECK(!ok.load());

        ms.advance(1s); // fires exactly once
        DSBOX_IPC_CHECK_EQ(calls.load(), 1);
    }

    void testReplyCancelsTheTimeout()
    {
        ManualScheduler ms;
        IpcChannelOptions options;
        options.scheduler = ms.scheduler;

        IpcChannel* aPtr = nullptr;
        IpcChannel* bPtr = nullptr;
        auto a = std::make_shared<IpcChannel>(std::make_unique<LengthPrefixedFrameCodec>(),
            [&](std::span<const std::byte> bytes) { bPtr->onBytesReceived(bytes); }, options);
        auto b = std::make_shared<IpcChannel>(std::make_unique<LengthPrefixedFrameCodec>(),
            [&](std::span<const std::byte> bytes) { aPtr->onBytesReceived(bytes); });
        aPtr = a.get();
        bPtr = b.get();

        b->registerHandler("echo", [](std::vector<std::byte> payload) { return payload; });

        auto future = a->request("echo", toBytes("hi"), 10s);

        DSBOX_IPC_CHECK(future.wait_for(0ms) == std::future_status::ready);
        DSBOX_IPC_CHECK_EQ(toString(future.get()), "hi");

        // The answered request left no timer behind.
        DSBOX_IPC_CHECK(!ms.scheduler->nextDeadline().has_value());
    }

    void testDefaultTimeoutAndPerCallOverride()
    {
        ManualScheduler ms;
        IpcChannelOptions options;
        options.scheduler = ms.scheduler;
        options.defaultTimeout = 50ms;
        auto channel = silentChannel(options);

        std::atomic<int> defaulted{ 0 };
        std::atomic<int> unbounded{ 0 };
        std::atomic<int> longer{ 0 };

        channel->requestAsync("x", {}, [&](bool, std::vector<std::byte>) { ++defaulted; });             // channel default: 50ms
        channel->requestAsync("x", {}, [&](bool, std::vector<std::byte>) { ++unbounded; }, 0ms);        // explicit "no timeout"
        channel->requestAsync("x", {}, [&](bool, std::vector<std::byte>) { ++longer; }, 200ms);         // explicit, longer

        ms.advance(50ms);
        DSBOX_IPC_CHECK_EQ(defaulted.load(), 1);
        DSBOX_IPC_CHECK_EQ(unbounded.load(), 0);
        DSBOX_IPC_CHECK_EQ(longer.load(), 0);

        ms.advance(150ms);
        DSBOX_IPC_CHECK_EQ(longer.load(), 1);
        DSBOX_IPC_CHECK_EQ(unbounded.load(), 0); // never times out on its own...

        channel->close();
        DSBOX_IPC_CHECK_EQ(unbounded.load(), 1); // ...only close() fails it
    }

    void testCloseFailsTimedRequestOnceAndCancelsItsTimer()
    {
        ManualScheduler ms;
        IpcChannelOptions options;
        options.scheduler = ms.scheduler;
        auto channel = silentChannel(options);

        std::atomic<int> calls{ 0 };
        std::atomic<bool> ok{ true };
        channel->requestAsync("x", {}, [&](bool wasOk, std::vector<std::byte>)
        {
            ++calls;
            ok = wasOk;
        }, 1h);

        DSBOX_IPC_CHECK(ms.scheduler->nextDeadline().has_value());

        channel->close();
        DSBOX_IPC_CHECK_EQ(calls.load(), 1);
        DSBOX_IPC_CHECK(!ok.load());
        DSBOX_IPC_CHECK(!ms.scheduler->nextDeadline().has_value());

        ms.advance(2h);
        DSBOX_IPC_CHECK_EQ(calls.load(), 1); // not failed a second time
    }

    void testDestroyingChannelLeavesNothingBehindOnSharedScheduler()
    {
        ManualScheduler ms;
        IpcChannelOptions options;
        options.scheduler = ms.scheduler;
        std::atomic<int> calls{ 0 };

        {
            auto channel = silentChannel(options);
            channel->requestAsync("x", {}, [&](bool, std::vector<std::byte>) { ++calls; }, 1h);
        } // destructor -> close() fails it and cancels the timer

        DSBOX_IPC_CHECK_EQ(calls.load(), 1);
        DSBOX_IPC_CHECK(!ms.scheduler->nextDeadline().has_value());

        ms.advance(2h); // nothing may touch the dead channel
        DSBOX_IPC_CHECK_EQ(calls.load(), 1);
    }

    void testTimeoutWorksWithoutAnInjectedScheduler()
    {
        // No scheduler supplied: the channel makes a private one, with a
        // real timer thread.
        auto channel = silentChannel();

        std::promise<bool> result;
        auto future = result.get_future();
        channel->requestAsync("x", {}, [&](bool ok, std::vector<std::byte>) { result.set_value(ok); }, 30ms);

        DSBOX_IPC_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_IPC_CHECK(!future.get());
    }

    void testFutureRequestTimesOut()
    {
        auto channel = silentChannel();
        auto future = channel->request("x", {}, 30ms);

        DSBOX_IPC_CHECK(future.wait_for(5s) == std::future_status::ready);

        bool threw = false;
        try { future.get(); }
        catch (const std::runtime_error&) { threw = true; }
        DSBOX_IPC_CHECK(threw);
    }

    void testStoppedSharedSchedulerFallsBackToPrivateOne()
    {
        ManualScheduler ms;
        ms.scheduler->stop(); // e.g. the application is shutting down

        IpcChannelOptions options;
        options.scheduler = ms.scheduler;
        auto channel = silentChannel(options);

        std::promise<bool> result;
        auto future = result.get_future();
        channel->requestAsync("x", {}, [&](bool ok, std::vector<std::byte>) { result.set_value(ok); }, 30ms);

        // The timeout is not lost just because the shared scheduler is gone.
        DSBOX_IPC_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_IPC_CHECK(!future.get());
    }

    void testCloseFromInsideTimeoutCallbackDoesNotDeadlock()
    {
        // The failure callback runs on the private scheduler's timer
        // thread; closing the channel from there makes close() stop that
        // same thread's scheduler.
        auto channel = silentChannel();
        std::promise<void> done;
        auto future = done.get_future();

        channel->requestAsync("x", {}, [&](bool, std::vector<std::byte>)
        {
            channel->close();
            done.set_value();
        }, 20ms);

        DSBOX_IPC_CHECK(future.wait_for(5s) == std::future_status::ready);
    }

    void testTimeoutsRacingWithRepliesFireExactlyOnce()
    {
        // Each request gets a 5ms timeout; half of them are answered at
        // a random moment between 0 and 8ms, so replies and timeouts keep
        // colliding. Whichever wins, the callback must run exactly once.
        constexpr int kRequests = 300;

        auto channel = silentChannel();
        LengthPrefixedFrameCodec codec;

        std::vector<std::atomic<int>> calls(kRequests);
        for (auto& c : calls)
            c = 0;

        for (int i = 0; i < kRequests; ++i)
        {
            const std::uint64_t id = channel->requestAsync(
                "x", {}, [&calls, i](bool, std::vector<std::byte>) { ++calls[i]; }, 5ms);

            if (i % 2 == 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds((i * 37) % 8000));

                Frame reply;
                reply.kind = FrameKind::Response;
                reply.id = id;
                channel->onBytesReceived(codec.encode(reply));
            }
        }

        DSBOX_IPC_CHECK(waitUntil([&]
        {
            for (auto& c : calls)
                if (c.load() == 0)
                    return false;
            return true;
        }));

        std::this_thread::sleep_for(50ms); // a double invocation would show up by now

        int wrong = 0;
        for (auto& c : calls)
            if (c.load() != 1)
                ++wrong;

        DSBOX_IPC_CHECK_EQ(wrong, 0);
    }
}

void runIpcChannelTests()
{
    runTest("RequestResponseRoundTrip", testRequestResponseRoundTrip);
    runTest("NotifyDeliveredWithoutResponse", testNotifyDeliveredWithoutResponse);
    runTest("RequestToUnknownTargetFails", testRequestToUnknownTargetFails);
    runTest("DuplicateHandlerRegistrationThrows", testDuplicateHandlerRegistrationThrows);
    runTest("CloseFailsPendingRequests", testCloseFailsPendingRequests);
    runTest("RequestTimesOutOnInjectedScheduler", testRequestTimesOutOnInjectedScheduler);
    runTest("ReplyCancelsTheTimeout", testReplyCancelsTheTimeout);
    runTest("DefaultTimeoutAndPerCallOverride", testDefaultTimeoutAndPerCallOverride);
    runTest("CloseFailsTimedRequestOnceAndCancelsItsTimer", testCloseFailsTimedRequestOnceAndCancelsItsTimer);
    runTest("DestroyingChannelLeavesNothingBehindOnSharedScheduler", testDestroyingChannelLeavesNothingBehindOnSharedScheduler);
    runTest("TimeoutWorksWithoutAnInjectedScheduler", testTimeoutWorksWithoutAnInjectedScheduler);
    runTest("FutureRequestTimesOut", testFutureRequestTimesOut);
    runTest("StoppedSharedSchedulerFallsBackToPrivateOne", testStoppedSharedSchedulerFallsBackToPrivateOne);
    runTest("CloseFromInsideTimeoutCallbackDoesNotDeadlock", testCloseFromInsideTimeoutCallbackDoesNotDeadlock);
    runTest("TimeoutsRacingWithRepliesFireExactlyOnce", testTimeoutsRacingWithRepliesFireExactlyOnce);
}
