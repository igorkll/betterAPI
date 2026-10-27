#include "test_framework.hpp"

#include <dsbox_net/network_manager.hpp>

#include <dsbox_scheduler/clock.hpp>
#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

using namespace dsbox_net_tests;
using namespace dsbox_net;
using namespace std::chrono_literals;

namespace
{
    // A trivial IModule, used only for its identity as an `owner`
    // pointer in the tests below -- never registered with a real
    // dsbox::ModuleManager/Context, which is fine per owner.hpp's own
    // docs (the pointer is never dereferenced except to read name() at
    // registration time).
    class FakeModule final : public dsbox::IModule
    {
    public:
        explicit FakeModule(std::string moduleName) : m_name(std::move(moduleName)) {}

        std::string_view name() const override
        {
            return m_name;
        }

    private:
        std::string m_name;
    };

    // Responds synchronously (from inside start(), on the calling
    // thread) with whatever result `respond` produces for that call.
    class ScriptedHandler final : public IProtocolHandler
    {
    public:
        using Responder = std::function<FetchResult(const Request&)>;

        explicit ScriptedHandler(Responder responder) : m_responder(std::move(responder)) {}

        void start(Request request, FetchCallback done) override
        {
            ++callCount;
            done(m_responder(request));
        }

        std::atomic<int> callCount{ 0 };

    private:
        Responder m_responder;
    };

    FetchResult makeSuccess(std::string bodyText = {})
    {
        FetchResult result;
        result.response.status = 200;
        result.response.statusClass = StatusClass::Success;

        if (!bodyText.empty())
        {
            auto bytes = std::vector<std::byte>(
                reinterpret_cast<const std::byte*>(bodyText.data()),
                reinterpret_cast<const std::byte*>(bodyText.data()) + bodyText.size());

            auto pipe = BodyPipe::create();
            auto writer = pipe.writer();
            writer.write(bytes);
            writer.finish();

            result.response.body = pipe.reader();
        }

        return result;
    }

    FetchResult blockingCall(NetworkManager& manager, Request request)
    {
        std::promise<FetchResult> promise;
        auto future = promise.get_future();

        manager.fetch(std::move(request), [&promise](FetchResult result)
        {
            promise.set_value(std::move(result));
        });

        return future.get();
    }

    void testFetchRoutesToRegisteredHandler()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&)
        {
            return makeSuccess("hi");
        });

        manager.registerHandler("http", handler);

        Request request;
        request.url = *Url::parse("http://example.com/");

        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(result.ok());
        DSBOX_NET_CHECK_EQ(result.response.status, 200);
        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 1);
    }

    void testFetchUnsupportedSchemeFails()
    {
        NetworkManager manager;

        Request request;
        request.url = *Url::parse("ftp://example.com/");

        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(!result.ok());
        DSBOX_NET_CHECK(result.error->code == NetErrorCode::UnsupportedScheme);
    }

    void testDuplicateSchemeRegistrationThrows()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });

        manager.registerHandler("http", handler);

        bool threw = false;
        try
        {
            manager.registerHandler("http", handler);
        }
        catch (const std::runtime_error&)
        {
            threw = true;
        }

        DSBOX_NET_CHECK(threw);
    }

    void testUnregisterHandlerStopsRouting()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler);

        DSBOX_NET_CHECK(manager.unregisterHandler("http"));
        DSBOX_NET_CHECK(!manager.supports("http"));

        Request request;
        request.url = *Url::parse("http://example.com/");
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(!result.ok());
        DSBOX_NET_CHECK(result.error->code == NetErrorCode::UnsupportedScheme);
    }

    void testUnregisterOwnerRemovesEverySchemeItOwns()
    {
        NetworkManager manager;
        FakeModule owner("test.owner");

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler, &owner);
        manager.registerHandler("https", handler, &owner);
        manager.registerHandler("ftp", handler, dsbox::Anonymous);

        const auto removed = manager.unregisterOwner(&owner);

        DSBOX_NET_CHECK_EQ(removed, static_cast<std::size_t>(2));
        DSBOX_NET_CHECK(!manager.supports("http"));
        DSBOX_NET_CHECK(!manager.supports("https"));
        DSBOX_NET_CHECK(manager.supports("ftp")); // registered anonymously, untouched
    }

    void testSchemeMatchingIsCaseInsensitive()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("HTTP", handler);

        DSBOX_NET_CHECK(manager.supports("http"));

        Request request;
        request.url = *Url::parse("http://example.com/");
        auto result = blockingCall(manager, request);
        DSBOX_NET_CHECK(result.ok());
    }

    void testInterceptorCanShortCircuitWithoutCallingNext()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler);

        struct ShortCircuitInterceptor final : IInterceptor
        {
            void intercept(Request, const Next&, FetchCallback done) override
            {
                FetchResult cached;
                cached.response.status = 304;
                cached.response.statusClass = StatusClass::Success;
                done(std::move(cached));
            }
        };

        manager.addInterceptor(std::make_shared<ShortCircuitInterceptor>());

        Request request;
        request.url = *Url::parse("http://example.com/");
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(result.ok());
        DSBOX_NET_CHECK_EQ(result.response.status, 304);
        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 0); // never reached
    }

    void testInterceptorsRunInOrderAndCanModifyRequest()
    {
        NetworkManager manager;
        std::string observedHeader;

        auto handler = std::make_shared<ScriptedHandler>([&](const Request& request)
        {
            observedHeader = std::string(request.metadata.get("X-Trace").value_or("<missing>"));
            return makeSuccess();
        });
        manager.registerHandler("http", handler);

        struct TaggingInterceptor final : IInterceptor
        {
            std::string tag;

            void intercept(Request request, const Next& next, FetchCallback done) override
            {
                std::string existing = std::string(request.metadata.get("X-Trace").value_or(""));
                request.metadata.set("X-Trace", existing + tag);
                next(std::move(request), std::move(done));
            }
        };

        auto first = std::make_shared<TaggingInterceptor>();
        first->tag = "A";
        auto second = std::make_shared<TaggingInterceptor>();
        second->tag = "B";

        manager.addInterceptor(first);
        manager.addInterceptor(second);

        Request request;
        request.url = *Url::parse("http://example.com/");
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(result.ok());
        DSBOX_NET_CHECK_EQ(observedHeader, "AB");
    }

    void testRemoveInterceptorStopsItFromRunning()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler);

        struct CountingInterceptor final : IInterceptor
        {
            std::atomic<int> calls{ 0 };

            void intercept(Request request, const Next& next, FetchCallback done) override
            {
                ++calls;
                next(std::move(request), std::move(done));
            }
        };

        auto interceptor = std::make_shared<CountingInterceptor>();
        const auto id = manager.addInterceptor(interceptor);

        Request request;
        request.url = *Url::parse("http://example.com/");
        blockingCall(manager, request);

        DSBOX_NET_CHECK(manager.removeInterceptor(id));
        blockingCall(manager, request);

        DSBOX_NET_CHECK_EQ(interceptor->calls.load(), 1);
    }

    void testRedirectIsFollowedAndRerunsInterceptorChain()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request& request)
        {
            if (request.url.path == "/old")
            {
                FetchResult result;
                result.response.status = 302;
                result.response.redirect = Redirect{ *request.url.resolve("/new"), "GET", false };
                return result;
            }

            return makeSuccess("landed");
        });
        manager.registerHandler("http", handler);

        struct CountingInterceptor final : IInterceptor
        {
            std::atomic<int> calls{ 0 };

            void intercept(Request request, const Next& next, FetchCallback done) override
            {
                ++calls;
                next(std::move(request), std::move(done));
            }
        };

        auto counter = std::make_shared<CountingInterceptor>();
        manager.addInterceptor(counter);

        Request request;
        request.url = *Url::parse("http://example.com/old");
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(result.ok());
        DSBOX_NET_CHECK_EQ(result.response.status, 200);
        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 2); // original hop + redirected hop
        DSBOX_NET_CHECK_EQ(counter->calls.load(), 2); // ran again on the redirected hop
    }

    // Serves a redirect from /old to `target`, then records what the
    // redirected hop looked like.
    struct RedirectProbe
    {
        std::string seenAuthorization = "<unset>";
        std::string seenCookie = "<unset>";
        std::string seenMethod;
    };

    RedirectProbe runRedirectProbe(NetOptions options, const std::string& from, const std::string& to, std::string redirectMethod)
    {
        NetworkManager manager(options);
        RedirectProbe probe;

        auto handler = std::make_shared<ScriptedHandler>([&](const Request& request)
        {
            if (request.url.path == "/old")
            {
                FetchResult result;
                result.response.status = 302;
                result.response.redirect = Redirect{ *Url::parse(to), redirectMethod, false };
                return result;
            }

            probe.seenAuthorization = std::string(request.metadata.get("Authorization").value_or("<none>"));
            probe.seenCookie = std::string(request.metadata.get("Cookie").value_or("<none>"));
            probe.seenMethod = request.method;
            return makeSuccess();
        });
        manager.registerHandler("http", handler);
        manager.registerHandler("https", handler);

        Request request;
        request.url = *Url::parse(from);
        request.method = "POST";
        request.metadata.set("Authorization", "Bearer secret");
        request.metadata.set("Cookie", "sid=1");

        blockingCall(manager, request);
        return probe;
    }

    void testCrossOriginRedirectStripsCredentials()
    {
        auto probe = runRedirectProbe({}, "http://a.example/old", "http://b.example/new", "GET");

        DSBOX_NET_CHECK_EQ(probe.seenAuthorization, "<none>");
        DSBOX_NET_CHECK_EQ(probe.seenCookie, "<none>");
    }

    void testSchemeChangeIsCrossOriginToo()
    {
        auto probe = runRedirectProbe({}, "http://a.example/old", "https://a.example/new", "GET");

        DSBOX_NET_CHECK_EQ(probe.seenAuthorization, "<none>");
    }

    void testSameOriginRedirectKeepsCredentials()
    {
        auto probe = runRedirectProbe({}, "http://a.example/old", "http://a.example/new", "GET");

        DSBOX_NET_CHECK_EQ(probe.seenAuthorization, "Bearer secret");
        DSBOX_NET_CHECK_EQ(probe.seenCookie, "sid=1");
    }

    void testStrippingCanBeDisabled()
    {
        NetOptions options;
        options.stripCredentialsOnCrossOriginRedirect = false;

        auto probe = runRedirectProbe(options, "http://a.example/old", "http://b.example/new", "GET");

        DSBOX_NET_CHECK_EQ(probe.seenAuthorization, "Bearer secret");
    }

    void testRedirectWithEmptyMethodKeepsOriginalMethod()
    {
        auto probe = runRedirectProbe({}, "http://a.example/old", "http://a.example/new", "");

        DSBOX_NET_CHECK_EQ(probe.seenMethod, "POST");
    }

    void testTooManyRedirectsFails()
    {
        NetworkManager manager(NetOptions{ .maxRedirects = 2 });

        auto handler = std::make_shared<ScriptedHandler>([](const Request& request)
        {
            FetchResult result;
            result.response.status = 302;
            // Always redirects to itself -- an infinite loop that
            // maxRedirects must cut off.
            result.response.redirect = Redirect{ request.url, request.method, false };
            return result;
        });
        manager.registerHandler("http", handler);

        Request request;
        request.url = *Url::parse("http://example.com/loop");
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(!result.ok());
        DSBOX_NET_CHECK(result.error->code == NetErrorCode::TooManyRedirects);
    }

    void testPreCancelledRequestFailsWithoutCallingHandler()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler);

        CancellationSource source;
        source.cancel(NetErrorCode::Cancelled);

        Request request;
        request.url = *Url::parse("http://example.com/");
        request.cancel = source.token();

        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(!result.ok());
        DSBOX_NET_CHECK(result.error->code == NetErrorCode::Cancelled);
        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 0);
    }

    void testInvalidUrlFailsWithoutCallingHandler()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler);

        Request request; // default Url has an empty scheme
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(!result.ok());
        DSBOX_NET_CHECK(result.error->code == NetErrorCode::InvalidUrl);
        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 0);
    }

    void testFutureOverloadResolvesWithResult()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess("ok"); });
        manager.registerHandler("http", handler);

        Request request;
        request.url = *Url::parse("http://example.com/");

        auto future = manager.fetch(std::move(request));
        auto result = future.get();

        DSBOX_NET_CHECK(result.ok());
    }

    void testResponseBodyIsReadableByCaller()
    {
        NetworkManager manager;

        auto handler = std::make_shared<ScriptedHandler>([](const Request&)
        {
            return makeSuccess("payload");
        });
        manager.registerHandler("http", handler);

        Request request;
        request.url = *Url::parse("http://example.com/");
        auto result = blockingCall(manager, request);

        DSBOX_NET_CHECK(result.ok());
        DSBOX_NET_CHECK(result.response.body != nullptr);

        auto bytes = readAll(*result.response.body, 1024);
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        DSBOX_NET_CHECK_EQ(text, "payload");
    }

    // -- request timeouts ----------------------------------------------------

    // Accepts every request and then never answers by itself -- it only
    // completes a fetch when its cancellation token fires, reporting the
    // token's reason, which is exactly what a real protocol handler is
    // required to do (see IProtocolHandler).
    class HangingHandler final : public IProtocolHandler
    {
    public:
        void start(Request request, FetchCallback done) override
        {
            ++callCount;

            auto subscription = request.cancel.onCancel(
                [token = request.cancel, done = std::move(done)]() mutable
                {
                    done(FetchResult{ NetError{ token.reason(), "cancelled" }, {} });
                });

            std::lock_guard<std::mutex> lock(m_mutex);
            m_subscriptions.push_back(std::move(subscription));
        }

        std::atomic<int> callCount{ 0 };

    private:
        std::mutex m_mutex;
        std::vector<Subscription> m_subscriptions; // dropping one would unhook its fetch
    };

    // A scheduler driven by hand: manual clock, no timer thread, callbacks
    // run inline inside runDue(). Tests using it never sleep.
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

    Request timedRequest(std::chrono::milliseconds timeout)
    {
        Request request;
        request.url = *Url::parse("http://example.com/");
        request.timeout = timeout;
        return request;
    }

    bool isReady(std::future<FetchResult>& future)
    {
        return future.wait_for(0ms) == std::future_status::ready;
    }

    void testTimeoutCancelsHungFetchOnInjectedScheduler()
    {
        ManualScheduler ms;
        NetworkManager manager;
        manager.useScheduler(ms.scheduler);

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        auto future = manager.fetch(timedRequest(100ms));
        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 1);
        DSBOX_NET_CHECK(!isReady(future));

        ms.advance(99ms);
        DSBOX_NET_CHECK(!isReady(future));

        ms.advance(1ms);
        DSBOX_NET_CHECK(isReady(future));

        auto result = future.get();
        DSBOX_NET_CHECK(!result.ok());
        DSBOX_NET_CHECK(result.error->code == NetErrorCode::Timeout);
    }

    void testCompletedFetchCancelsItsTimeoutTimer()
    {
        ManualScheduler ms;
        NetworkManager manager;
        manager.useScheduler(ms.scheduler);

        auto handler = std::make_shared<ScriptedHandler>([](const Request&) { return makeSuccess(); });
        manager.registerHandler("http", handler);

        auto result = blockingCall(manager, timedRequest(10s));
        DSBOX_NET_CHECK(result.ok());

        // The fetch finished long before its timeout: the timer must not
        // be left pending (the old per-fetch sleeper thread was).
        DSBOX_NET_CHECK(!ms.scheduler->nextDeadline().has_value());
    }

    void testFetchWithoutTimeoutArmsNoTimer()
    {
        ManualScheduler ms;
        NetworkManager manager;
        manager.useScheduler(ms.scheduler);

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        Request request;
        request.url = *Url::parse("http://example.com/");
        manager.fetch(std::move(request), [](FetchResult) {});

        DSBOX_NET_CHECK_EQ(handler->callCount.load(), 1);
        DSBOX_NET_CHECK(!ms.scheduler->nextDeadline().has_value());
    }

    void testManagerDefaultTimeoutApplies()
    {
        ManualScheduler ms;
        NetworkManager manager(NetOptions{ .defaultTimeout = 50ms });
        manager.useScheduler(ms.scheduler);

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        Request request; // Request::timeout == 0 -> the manager's default
        request.url = *Url::parse("http://example.com/");
        auto future = manager.fetch(std::move(request));

        ms.advance(50ms);
        DSBOX_NET_CHECK(isReady(future));
        DSBOX_NET_CHECK(future.get().error->code == NetErrorCode::Timeout);
    }

    void testCallerCancellationBeatsTimeoutAndDisarmsIt()
    {
        ManualScheduler ms;
        NetworkManager manager;
        manager.useScheduler(ms.scheduler);

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        CancellationSource caller;
        Request request = timedRequest(1h);
        request.cancel = caller.token();
        auto future = manager.fetch(std::move(request));

        DSBOX_NET_CHECK(ms.scheduler->nextDeadline().has_value());
        DSBOX_NET_CHECK(!isReady(future));

        caller.cancel(NetErrorCode::Cancelled);

        DSBOX_NET_CHECK(isReady(future));
        DSBOX_NET_CHECK(future.get().error->code == NetErrorCode::Cancelled);
        DSBOX_NET_CHECK(!ms.scheduler->nextDeadline().has_value()); // the hour-long timer is gone
    }

    void testTimeoutWorksWithoutAnInjectedScheduler()
    {
        // No scheduler supplied: the manager uses a private one, with a
        // real timer thread.
        NetworkManager manager;

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        auto future = manager.fetch(timedRequest(30ms));

        DSBOX_NET_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_NET_CHECK(future.get().error->code == NetErrorCode::Timeout);
    }

    void testStoppedSharedSchedulerFallsBackToPrivateOne()
    {
        ManualScheduler ms;
        ms.scheduler->stop(); // e.g. the application is shutting down

        NetworkManager manager;
        manager.useScheduler(ms.scheduler);

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        auto future = manager.fetch(timedRequest(30ms));

        // The timeout is not lost just because the shared scheduler is gone.
        DSBOX_NET_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_NET_CHECK(future.get().error->code == NetErrorCode::Timeout);
    }

    void testLettingGoOfTheSharedSchedulerUsesThePrivateOne()
    {
        ManualScheduler ms;
        NetworkManager manager;
        manager.useScheduler(ms.scheduler);
        manager.useScheduler(nullptr); // what NetModule::onStop() does

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        auto future = manager.fetch(timedRequest(30ms));

        DSBOX_NET_CHECK(!ms.scheduler->nextDeadline().has_value()); // not armed on the released one
        DSBOX_NET_CHECK(future.wait_for(5s) == std::future_status::ready);
        DSBOX_NET_CHECK(future.get().error->code == NetErrorCode::Timeout);
    }

    void testManyConcurrentTimedFetchesAllTimeOut()
    {
        NetworkManager manager;

        auto handler = std::make_shared<HangingHandler>();
        manager.registerHandler("http", handler);

        std::vector<std::future<FetchResult>> futures;
        for (int i = 0; i < 200; ++i)
            futures.push_back(manager.fetch(timedRequest(std::chrono::milliseconds(10 + i % 20))));

        int timedOut = 0;
        for (auto& future : futures)
        {
            if (future.wait_for(10s) != std::future_status::ready)
                continue;

            const auto result = future.get();
            if (!result.ok() && result.error->code == NetErrorCode::Timeout)
                ++timedOut;
        }

        DSBOX_NET_CHECK_EQ(timedOut, 200);
    }

    void testDestroyingManagerWithPendingTimedFetchDoesNotHang()
    {
        {
            NetworkManager manager;

            auto handler = std::make_shared<HangingHandler>();
            manager.registerHandler("http", handler);

            manager.fetch(timedRequest(1h), [](FetchResult) {});
        } // the hour-long timer must not keep the manager's destructor waiting

        DSBOX_NET_CHECK(true);
    }
}

void runNetworkManagerTests()
{
    runTest("testFetchRoutesToRegisteredHandler", testFetchRoutesToRegisteredHandler);
    runTest("testFetchUnsupportedSchemeFails", testFetchUnsupportedSchemeFails);
    runTest("testDuplicateSchemeRegistrationThrows", testDuplicateSchemeRegistrationThrows);
    runTest("testUnregisterHandlerStopsRouting", testUnregisterHandlerStopsRouting);
    runTest("testUnregisterOwnerRemovesEverySchemeItOwns", testUnregisterOwnerRemovesEverySchemeItOwns);
    runTest("testSchemeMatchingIsCaseInsensitive", testSchemeMatchingIsCaseInsensitive);
    runTest("testInterceptorCanShortCircuitWithoutCallingNext", testInterceptorCanShortCircuitWithoutCallingNext);
    runTest("testInterceptorsRunInOrderAndCanModifyRequest", testInterceptorsRunInOrderAndCanModifyRequest);
    runTest("testRemoveInterceptorStopsItFromRunning", testRemoveInterceptorStopsItFromRunning);
    runTest("testRedirectIsFollowedAndRerunsInterceptorChain", testRedirectIsFollowedAndRerunsInterceptorChain);
    runTest("testTooManyRedirectsFails", testTooManyRedirectsFails);
    runTest("testCrossOriginRedirectStripsCredentials", testCrossOriginRedirectStripsCredentials);
    runTest("testSchemeChangeIsCrossOriginToo", testSchemeChangeIsCrossOriginToo);
    runTest("testSameOriginRedirectKeepsCredentials", testSameOriginRedirectKeepsCredentials);
    runTest("testStrippingCanBeDisabled", testStrippingCanBeDisabled);
    runTest("testRedirectWithEmptyMethodKeepsOriginalMethod", testRedirectWithEmptyMethodKeepsOriginalMethod);
    runTest("testPreCancelledRequestFailsWithoutCallingHandler", testPreCancelledRequestFailsWithoutCallingHandler);
    runTest("testInvalidUrlFailsWithoutCallingHandler", testInvalidUrlFailsWithoutCallingHandler);
    runTest("testFutureOverloadResolvesWithResult", testFutureOverloadResolvesWithResult);
    runTest("testResponseBodyIsReadableByCaller", testResponseBodyIsReadableByCaller);
    runTest("testTimeoutCancelsHungFetchOnInjectedScheduler", testTimeoutCancelsHungFetchOnInjectedScheduler);
    runTest("testCompletedFetchCancelsItsTimeoutTimer", testCompletedFetchCancelsItsTimeoutTimer);
    runTest("testFetchWithoutTimeoutArmsNoTimer", testFetchWithoutTimeoutArmsNoTimer);
    runTest("testManagerDefaultTimeoutApplies", testManagerDefaultTimeoutApplies);
    runTest("testCallerCancellationBeatsTimeoutAndDisarmsIt", testCallerCancellationBeatsTimeoutAndDisarmsIt);
    runTest("testTimeoutWorksWithoutAnInjectedScheduler", testTimeoutWorksWithoutAnInjectedScheduler);
    runTest("testStoppedSharedSchedulerFallsBackToPrivateOne", testStoppedSharedSchedulerFallsBackToPrivateOne);
    runTest("testLettingGoOfTheSharedSchedulerUsesThePrivateOne", testLettingGoOfTheSharedSchedulerUsesThePrivateOne);
    runTest("testManyConcurrentTimedFetchesAllTimeOut", testManyConcurrentTimedFetchesAllTimeOut);
    runTest("testDestroyingManagerWithPendingTimedFetchDoesNotHang", testDestroyingManagerWithPendingTimedFetchDoesNotHang);
}
