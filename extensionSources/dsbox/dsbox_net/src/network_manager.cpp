#include <dsbox_net/network_manager.hpp>

#include <dsbox_scheduler/executor.hpp>
#include <dsbox_scheduler/scheduler.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

namespace dsbox_net
{
    std::string NetworkManager::normalizeScheme(std::string_view scheme)
    {
        std::string result(scheme);

        for (char& c : result)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        return result;
    }

    namespace
    {
        bool sameOrigin(const Url& a, const Url& b)
        {
            return a.scheme == b.scheme
                && a.host == b.host
                && a.effectivePort() == b.effectivePort();
        }
    }

    NetworkManager::NetworkManager(NetOptions options)
        : m_options(options)
    {
    }

    /*
     * Scheme registry -- see network_manager.hpp for why this is just a
     * thin wrapper over m_registry's own registerHandler/unregisterHandler/
     * unregisterOwner/listHandlers.
     */

    void NetworkManager::registerHandler(
        std::string scheme,
        std::shared_ptr<IProtocolHandler> handler,
        const dsbox::IModule* owner)
    {
        const std::string normalized = normalizeScheme(scheme);

        // Captured by shared_ptr: as long as this registration entry
        // exists, `handler` stays alive through it -- exactly the
        // lifetime unregisterHandler()/unregisterOwner() are documented
        // to end (see IProtocolHandler's own docs on what its owning
        // module must additionally guarantee about in-flight calls).
        m_registry.registerHandler<detail::ProtocolSignature>(
            normalized,
            [handler](detail::FetchInvocation invocation)
            {
                handler->start(std::move(invocation.request), std::move(invocation.done));
            },
            owner);
    }

    bool NetworkManager::unregisterHandler(std::string_view scheme)
    {
        return m_registry.unregisterHandler<detail::ProtocolSignature>(normalizeScheme(scheme));
    }

    std::size_t NetworkManager::unregisterOwner(const dsbox::IModule* owner)
    {
        return m_registry.unregisterOwner(owner);
    }

    bool NetworkManager::supports(std::string_view scheme) const
    {
        return m_registry.hasHandler<detail::ProtocolSignature>(normalizeScheme(scheme));
    }

    std::vector<std::string> NetworkManager::schemes() const
    {
        std::vector<std::string> result;

        for (const auto& handlerInfo : m_registry.listHandlers())
            result.push_back(handlerInfo.name);

        return result;
    }

    /*
     * Interceptors
     */

    std::uint64_t NetworkManager::addInterceptor(
        std::shared_ptr<IInterceptor> interceptor,
        const dsbox::IModule* owner)
    {
        std::lock_guard<std::mutex> lock(m_interceptorsMutex);

        const auto id = m_nextInterceptorId++;
        m_interceptors.push_back(InterceptorEntry{ id, std::move(interceptor), owner });

        return id;
    }

    bool NetworkManager::removeInterceptor(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(m_interceptorsMutex);

        const auto it = std::find_if(
            m_interceptors.begin(),
            m_interceptors.end(),
            [id](const InterceptorEntry& entry) { return entry.id == id; });

        if (it == m_interceptors.end())
            return false;

        m_interceptors.erase(it);
        return true;
    }

    /*
     * Fetching
     */

    void NetworkManager::fetch(Request request, FetchCallback done)
    {
        const std::chrono::milliseconds effectiveTimeout =
            request.timeout.count() > 0 ? request.timeout : m_options.defaultTimeout;

        if (effectiveTimeout.count() <= 0)
        {
            fetchHop(std::move(request), m_options.maxRedirects, std::move(done));
            return;
        }

        // A timeout applies to this fetch (including every redirect hop
        // -- see Request::timeout's own docs): fold it, and whatever
        // cancellation the caller already supplied, into one internal
        // CancellationSource that every hop's Request::cancel points to
        // instead.
        //
        // The timeout itself is one timer on a scheduler (the shared one
        // from useScheduler(), else this manager's private one), not a
        // thread of its own: no thread per timed fetch, and the timer is
        // cancelled -- and its captures released -- as soon as the fetch
        // completes instead of lingering for the whole timeout.
        auto source = std::make_shared<CancellationSource>();
        const CancellationToken callerToken = request.cancel;

        auto forwarding = std::make_shared<Subscription>(
            callerToken.onCancel([source, callerToken]
            {
                source->cancel(callerToken.reason());
            }));

        request.cancel = source->token();

        dsbox_scheduler::Callback onTimeout = [source]
        {
            source->cancel(NetErrorCode::Timeout);
        };

        dsbox_scheduler::TimerHandle timer;

        if (const auto shared = sharedScheduler())
            timer = shared->after(effectiveTimeout, onTimeout);

        if (!timer)
        {
            // No shared scheduler, or it has been stopped (application
            // shutting down): fall back to the private one rather than
            // let this fetch run without its timeout.
            timer = privateScheduler()->after(effectiveTimeout, std::move(onTimeout));
        }

        // Keeps `source` (so its token stays meaningful for the whole
        // fetch) and `forwarding` (so the caller-cancellation
        // subscription isn't torn down early) alive until `done` fires,
        // and cancels the timeout timer once it has -- whether the fetch
        // finished on its own or because that very timer fired (then
        // cancel() is simply a no-op).
        FetchCallback wrapped =
            [source, forwarding, timer, done = std::move(done)](FetchResult result) mutable
            {
                timer.cancel();
                done(std::move(result));
            };

        fetchHop(std::move(request), m_options.maxRedirects, std::move(wrapped));
    }

    void NetworkManager::useScheduler(std::shared_ptr<dsbox_scheduler::Scheduler> scheduler)
    {
        std::lock_guard<std::mutex> lock(m_schedulerMutex);
        m_scheduler = std::move(scheduler);
    }

    std::shared_ptr<dsbox_scheduler::Scheduler> NetworkManager::sharedScheduler() const
    {
        std::lock_guard<std::mutex> lock(m_schedulerMutex);
        return m_scheduler;
    }

    std::shared_ptr<dsbox_scheduler::Scheduler> NetworkManager::privateScheduler()
    {
        std::lock_guard<std::mutex> lock(m_schedulerMutex);

        if (!m_privateScheduler)
        {
            // A timeout only cancels a token: cheap and non-blocking, so
            // there is no need for a worker pool -- run it right on the
            // timer thread. (That is also what the per-fetch sleeper
            // thread this replaces did.)
            dsbox_scheduler::SchedulerOptions options;
            options.defaultExecutor = std::make_shared<dsbox_scheduler::InlineExecutor>();
            m_privateScheduler = std::make_shared<dsbox_scheduler::Scheduler>(std::move(options));
        }

        return m_privateScheduler;
    }

    std::future<FetchResult> NetworkManager::fetch(Request request)
    {
        auto promise = std::make_shared<std::promise<FetchResult>>();
        std::future<FetchResult> future = promise->get_future();

        fetch(std::move(request), [promise](FetchResult result) mutable
        {
            promise->set_value(std::move(result));
        });

        return future;
    }

    void NetworkManager::fetchHop(Request request, int hopsRemaining, FetchCallback done)
    {
        if (hopsRemaining < 0)
        {
            done(FetchResult{
                NetError{ NetErrorCode::TooManyRedirects, "Exceeded NetOptions::maxRedirects" },
                {}
            });
            return;
        }

        if (request.cancel.isCancelled())
        {
            done(FetchResult{
                NetError{ request.cancel.reason(), "Request was cancelled before it could be sent" },
                {}
            });
            return;
        }

        if (request.url.scheme.empty())
        {
            done(FetchResult{
                NetError{ NetErrorCode::InvalidUrl, "Request::url has no scheme" },
                {}
            });
            return;
        }

        runInterceptorChain(std::move(request), 0, hopsRemaining, std::move(done));
    }

    void NetworkManager::runInterceptorChain(
        Request request,
        std::size_t index,
        int hopsRemaining,
        FetchCallback done)
    {
        std::shared_ptr<IInterceptor> interceptor;

        {
            std::lock_guard<std::mutex> lock(m_interceptorsMutex);

            if (index < m_interceptors.size())
                interceptor = m_interceptors[index].interceptor;
        }

        if (interceptor)
        {
            Next next = [this, index, hopsRemaining](Request nextRequest, FetchCallback nextDone)
            {
                runInterceptorChain(std::move(nextRequest), index + 1, hopsRemaining, std::move(nextDone));
            };

            interceptor->intercept(std::move(request), next, std::move(done));
            return;
        }

        // End of the interceptor chain -- route to the protocol handler
        // for this hop's scheme.
        const std::string scheme = normalizeScheme(request.url.scheme);

        auto entry = m_registry.getHandler<detail::ProtocolSignature>(scheme).pin();

        if (!entry)
        {
            done(FetchResult{
                NetError{
                    NetErrorCode::UnsupportedScheme,
                    "No protocol handler registered for scheme '" + scheme + "'"
                },
                {}
            });
            return;
        }

        // Kept for a possible redirect continuation (see `wrapped`
        // below): Request is cheaply copyable (Url/Metadata are plain
        // data, `body` and `cancel` are shared_ptr/shared-state handles).
        Request carryForRedirect = request;

        FetchCallback wrapped =
            [this, hopsRemaining, carry = std::move(carryForRedirect), done = std::move(done)]
            (FetchResult result) mutable
            {
                if (!result.ok() || !result.response.redirect.has_value())
                {
                    done(std::move(result));
                    return;
                }

                if (hopsRemaining <= 0)
                {
                    done(FetchResult{
                        NetError{ NetErrorCode::TooManyRedirects, "Exceeded NetOptions::maxRedirects" },
                        {}
                    });
                    return;
                }

                Redirect redirect = std::move(*result.response.redirect);

                Request nextRequest = carry; // reuses metadata/timeout/cancel from this hop
                nextRequest.url = redirect.target;

                // An empty method means "keep the current one".
                if (!redirect.method.empty())
                    nextRequest.method = redirect.method;

                if (!redirect.keepBody)
                    nextRequest.body = nullptr;

                if (m_options.stripCredentialsOnCrossOriginRedirect
                    && !sameOrigin(carry.url, nextRequest.url))
                {
                    nextRequest.metadata.remove("Authorization");
                    nextRequest.metadata.remove("Proxy-Authorization");
                    nextRequest.metadata.remove("Cookie");
                }

                // Runs the FULL interceptor chain again for the new hop
                // -- see IInterceptor's own docs on why (cookies/auth
                // must be re-evaluated if the redirect crosses hosts).
                fetchHop(std::move(nextRequest), hopsRemaining - 1, std::move(done));
            };

        entry->function(detail::FetchInvocation{ std::move(request), std::move(wrapped) });
    }
}
