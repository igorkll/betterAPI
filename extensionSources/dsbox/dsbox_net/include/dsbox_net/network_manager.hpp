#pragma once

#include <dsbox_net/api.hpp>
#include <dsbox_net/interceptor.hpp>
#include <dsbox_net/protocol_handler.hpp>
#include <dsbox_net/request.hpp>
#include <dsbox_net/response.hpp>

#include <dsbox/communication_manager.hpp>
#include <dsbox/module.hpp>
#include <dsbox/owner.hpp>

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_scheduler
{
    class Scheduler;
}

namespace dsbox_net
{
    // Configuration for a single NetworkManager instance, fixed for its
    // whole lifetime (no setters -- construct a new instance, or go
    // through dsbox_net::NetModule/NetOptions loading from config, if
    // you need different behavior). See net_module.hpp for how these
    // are populated from a dsbox_config::PersistentConfigStore ("net"
    // section) when one is available, rather than being hardcoded by
    // whatever links this library.
    struct NetOptions
    {
        // Upper bound on redirect hops followed within one fetch() call
        // before it fails with TooManyRedirects. A Request/Redirect
        // cycle (A -> B -> A) is caught by this same counter, not by
        // cycle detection -- once it's exceeded, the fetch fails either
        // way.
        int maxRedirects = 20;

        // Used for Request::timeout == 0, i.e. "use the manager's
        // default". 0 here means "no timeout at all" -- matches this
        // project's usual convention (IOModule::maxQueueSize,
        // IpcChannelOptions::defaultTimeout) for "0 = unbounded".
        std::chrono::milliseconds defaultTimeout{ 0 };

        // Default buffering capacity for a BodyPipe created by a
        // protocol handler that doesn't have a more specific size in
        // mind. Purely advisory -- handlers are free to construct their
        // own BodyPipe::create() with a different capacity; this only
        // matters to code that reads it back out of the NetworkManager
        // it was given (see NetModule).
        std::size_t bodyPipeCapacity = 256 * 1024;

        // When a redirect hop changes origin (scheme, host or effective
        // port), drop Authorization, Proxy-Authorization and Cookie from
        // the request metadata the caller supplied before re-sending it,
        // so credentials meant for one site never leak to another. An
        // interceptor is still free to attach them again for the new
        // origin (they run again on every hop). Same-origin redirects
        // are left untouched.
        bool stripCredentialsOnCrossOriginRedirect = true;
    };

    namespace detail
    {
        // The payload/return-type pair every scheme "handler" agrees on
        // when registered on NetworkManager's private routing
        // CommunicationManager (see NetworkManager's own docs for why
        // it reuses that class instead of a bespoke map+mutex). Kept in
        // `detail` -- nothing outside NetworkManager needs to spell out
        // this signature directly.
        struct FetchInvocation
        {
            Request request;
            FetchCallback done;
        };

        using ProtocolSignature = void(FetchInvocation);
    }

    // Routes a Request to the IProtocolHandler registered for its
    // URL's scheme, running every registered IInterceptor first (and
    // again on each redirect hop), and following Response::redirect
    // itself so individual handlers never need to know about that
    // policy.
    //
    // Scheme registry: implemented as a private, dedicated
    // dsbox::CommunicationManager (NOT the module bus in Context::comm()
    // -- this one is exclusively internal bookkeeping and never visible
    // outside this class) used purely for its existing
    // registerHandler/unregisterHandler/unregisterOwner/listHandlers
    // machinery: a scheme is just a "handler name" with
    // detail::ProtocolSignature as its signature. This gets us, for
    // free and already battle-tested: rejection of a second handler for
    // the same scheme, safe (weak-referenced) removal that can't
    // dangle, and -- most usefully for a protocol module's own
    // lifecycle -- unregisterOwner(this) as the ENTIRE cleanup a module
    // needs in its onStop(), covering every scheme it registered in one
    // call, exactly the same one-liner IModule authors already reach
    // for on the shared bus.
    //
    // A protocol handler module is still responsible for making sure
    // every in-flight start() call it handed a `done` to has actually
    // invoked it before onStop() returns -- see IProtocolHandler's own
    // docs on why (the weak reference means unregisterHandler()/
    // unregisterOwner() do NOT wait for or cancel calls already in
    // progress, they only stop new ones from starting).
    //
    // Body model: response bodies are pulled (see body.hpp's IByteStream)
    // rather than pushed with pause/resume -- deliberately, to keep this
    // first version's contract simple. The trade-off this implies is a
    // thread parked in read() for as long as a caller is actively
    // reading a body; that is fine for a handful of concurrent transfers
    // (typical of a subprocess or tool talking HTTP) but would need
    // revisiting (a push model with backpressure signalled by return
    // value instead of blocking) before this library is used for, say,
    // a browser's tab with dozens of simultaneous in-flight resources.
    //
    // Thread safety: every public method is safe to call concurrently
    // from multiple threads.
    class DSBOX_NET_API NetworkManager
    {
    public:
        explicit NetworkManager(NetOptions options = {});

        NetworkManager(const NetworkManager&) = delete;
        NetworkManager& operator=(const NetworkManager&) = delete;

        const NetOptions& options() const noexcept
        {
            return m_options;
        }

        /*
         * Scheme registry
         */

        // Registers `handler` to answer for `scheme` (compared
        // case-insensitively; stored lowercase). Throws std::runtime_error
        // if a handler is already registered for this scheme. `owner`,
        // if supplied, is what unregisterOwner() below removes by --
        // pass `this` from inside the registering module, exactly like
        // dsbox::CommunicationManager::registerHandler's own `owner`
        // parameter.
        void registerHandler(
            std::string scheme,
            std::shared_ptr<IProtocolHandler> handler,
            const dsbox::IModule* owner = dsbox::Anonymous);

        // Removes the handler for `scheme`. Returns true if there was
        // one. Does NOT wait for or cancel any start() call already in
        // progress against it -- see the class-level note on what a
        // handler's owning module must do before this (or
        // unregisterOwner()) is called from onStop().
        bool unregisterHandler(std::string_view scheme);

        // Removes every scheme handler registered with this `owner`.
        // Returns how many were removed. The intended one-liner for a
        // protocol module's onStop().
        std::size_t unregisterOwner(const dsbox::IModule* owner);

        bool supports(std::string_view scheme) const;

        // Every scheme currently handled, lowercase, in no particular
        // order.
        std::vector<std::string> schemes() const;

        /*
         * Interceptors
         */

        // Appends `interceptor` to the end of the chain (see
        // IInterceptor's docs for what "the chain" means). Order
        // matters: interceptors run in the order they were added, each
        // deciding whether to call the next one. Returns an id usable
        // with removeInterceptor(); `owner` is recorded for parity with
        // the scheme registry but there is currently no bulk
        // removeInterceptorsOf() -- remove each id individually (a
        // module that adds interceptors typically adds few, and can
        // keep the ids as members).
        std::uint64_t addInterceptor(
            std::shared_ptr<IInterceptor> interceptor,
            const dsbox::IModule* owner = dsbox::Anonymous);

        bool removeInterceptor(std::uint64_t id);

        /*
         * Fetching
         */

        void fetch(Request request, FetchCallback done);

        // Convenience wrapper for callers who'd rather block/co_await
        // than write a callback -- mirrors dsbox_ipc::IpcChannel::request()
        // for the same reason. The future is satisfied from whatever
        // thread the last interceptor/protocol handler in the chain
        // calls `done` from; avoid calling get() from that same thread.
        std::future<FetchResult> fetch(Request request);

        /*
         * Timeouts
         */

        // Gives this manager a (typically shared) scheduler to run
        // request timeouts on: each fetch() with a timeout arms one
        // timer there, cancelled the moment the fetch completes. Optional
        // and replaceable at any time (pass nullptr to let go of it);
        // affects fetches started afterwards.
        //
        // Without one -- or if the one given has been stopped -- the
        // manager uses a small private scheduler of its own (one timer
        // thread for ALL its timed fetches, created on first use), so a
        // timeout is never silently lost. Cancellation callbacks run on
        // the private scheduler's timer thread; on a shared scheduler,
        // on its default executor.
        void useScheduler(std::shared_ptr<dsbox_scheduler::Scheduler> scheduler);

    private:
        struct InterceptorEntry
        {
            std::uint64_t id = 0;
            std::shared_ptr<IInterceptor> interceptor;
            const dsbox::IModule* owner = dsbox::Anonymous;
        };

        // One hop of a (possibly redirected) fetch. `hopsRemaining`
        // counts down; running out fails with TooManyRedirects.
        // `rootCancel` is the token every hop must additionally honor
        // (the caller-supplied Request::cancel from the original call,
        // or a manager-owned one if a timeout was set -- see
        // network_manager.cpp).
        void fetchHop(Request request, int hopsRemaining, FetchCallback done);

        void runInterceptorChain(
            Request request,
            std::size_t index,
            int hopsRemaining,
            FetchCallback done);

        static std::string normalizeScheme(std::string_view scheme);

        std::shared_ptr<dsbox_scheduler::Scheduler> sharedScheduler() const;
        std::shared_ptr<dsbox_scheduler::Scheduler> privateScheduler();

        NetOptions m_options;

        dsbox::CommunicationManager m_registry;

        mutable std::mutex m_interceptorsMutex;
        std::vector<InterceptorEntry> m_interceptors;
        std::uint64_t m_nextInterceptorId = 1;

        mutable std::mutex m_schedulerMutex;
        std::shared_ptr<dsbox_scheduler::Scheduler> m_scheduler;        // from useScheduler(), may be null
        std::shared_ptr<dsbox_scheduler::Scheduler> m_privateScheduler; // created lazily
    };
}
