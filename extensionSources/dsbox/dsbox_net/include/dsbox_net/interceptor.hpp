#pragma once

#include <dsbox_net/api.hpp>
#include <dsbox_net/protocol_handler.hpp>

namespace dsbox_net
{
    // Continuation an IInterceptor calls to hand the (possibly modified)
    // request on to the next interceptor in the chain, and eventually to
    // the protocol handler chosen for its scheme. Calling it more than
    // once, or not at all, is a misuse the interceptor is responsible
    // for avoiding -- NetworkManager does not guard against either (see
    // IInterceptor's own docs).
    using Next = std::function<void(Request, FetchCallback)>;

    // Cross-cutting policy applied around every fetch NetworkManager
    // performs, run again on EVERY hop of a redirected fetch (see
    // NetworkManager's docs) -- so a cookie jar or an auth header
    // attached here is correctly re-evaluated if a redirect crosses to
    // a different host, rather than blindly carried over from the first
    // hop. This is deliberately outside NetworkManager itself: CORS,
    // CSP, mixed-content checks, and cookie policy belong in a
    // browser-level interceptor built on top of this library, not baked
    // into dsbox_net (see the project's own scope notes).
    //
    // An interceptor MUST call exactly one of: `next` (to continue the
    // chain, optionally after modifying `request`), or `done` directly
    // (to short-circuit -- e.g. serve from cache, or reject with a
    // NetError -- without involving `next`/the protocol handler at all).
    // Whichever is called, it must be called exactly once and it may be
    // called asynchronously (from a different thread, later), exactly
    // like IProtocolHandler::start()'s own `done` contract.
    class DSBOX_NET_API IInterceptor
    {
    public:
        virtual ~IInterceptor() = default;

        virtual void intercept(Request request, const Next& next, FetchCallback done) = 0;
    };
}
