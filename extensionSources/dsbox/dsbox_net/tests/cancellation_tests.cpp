#include "test_framework.hpp"

#include <dsbox_net/cancellation.hpp>

#include <atomic>

using namespace dsbox_net_tests;
using dsbox_net::CancellationSource;
using dsbox_net::CancellationToken;
using dsbox_net::NetErrorCode;

namespace
{
    void testDefaultTokenNeverCancels()
    {
        CancellationToken token; // default-constructed
        DSBOX_NET_CHECK(!token.isCancelled());

        bool called = false;
        auto sub = token.onCancel([&] { called = true; });
        DSBOX_NET_CHECK(!called);
    }

    void testCancelFiresRegisteredCallback()
    {
        CancellationSource source;
        auto token = source.token();

        bool called = false;
        auto sub = token.onCancel([&] { called = true; });

        DSBOX_NET_CHECK(!called);
        source.cancel(NetErrorCode::Timeout);
        DSBOX_NET_CHECK(called);
        DSBOX_NET_CHECK(token.isCancelled());
        DSBOX_NET_CHECK(token.reason() == NetErrorCode::Timeout);
    }

    void testOnCancelAfterAlreadyCancelledFiresImmediately()
    {
        CancellationSource source;
        source.cancel(NetErrorCode::Cancelled);

        bool called = false;
        auto sub = source.token().onCancel([&] { called = true; });
        DSBOX_NET_CHECK(called);
    }

    void testFirstCancelReasonWins()
    {
        CancellationSource source;
        source.cancel(NetErrorCode::Timeout);
        source.cancel(NetErrorCode::Cancelled); // should be ignored

        DSBOX_NET_CHECK(source.token().reason() == NetErrorCode::Timeout);
    }

    void testSubscriptionRemovesCallbackOnDestruction()
    {
        CancellationSource source;
        bool called = false;

        {
            auto sub = source.token().onCancel([&] { called = true; });
            // sub destroyed here, before cancel()
        }

        source.cancel();
        DSBOX_NET_CHECK(!called);
    }

    void testMultipleTokensShareState()
    {
        CancellationSource source;
        CancellationToken a = source.token();
        CancellationToken b = source.token();

        source.cancel();

        DSBOX_NET_CHECK(a.isCancelled());
        DSBOX_NET_CHECK(b.isCancelled());
    }
}

void runCancellationTests()
{
    runTest("testDefaultTokenNeverCancels", testDefaultTokenNeverCancels);
    runTest("testCancelFiresRegisteredCallback", testCancelFiresRegisteredCallback);
    runTest("testOnCancelAfterAlreadyCancelledFiresImmediately", testOnCancelAfterAlreadyCancelledFiresImmediately);
    runTest("testFirstCancelReasonWins", testFirstCancelReasonWins);
    runTest("testSubscriptionRemovesCallbackOnDestruction", testSubscriptionRemovesCallbackOnDestruction);
    runTest("testMultipleTokensShareState", testMultipleTokensShareState);
}
