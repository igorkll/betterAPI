#pragma once

#include <dsbox_net/api.hpp>
#include <dsbox_net/request.hpp>
#include <dsbox_net/response.hpp>

#include <functional>

namespace dsbox_net
{
    using FetchCallback = std::function<void(FetchResult)>;

    // Contract every protocol module implements to answer for one or
    // more URL schemes -- HTTP/HTTPS, FTP, file://, whatever else this
    // project eventually adds. Registered with
    // NetworkManager::registerHandler(scheme, handler, owner); see that
    // class for how routing and redirects are layered on top of this.
    //
    // This library imposes no fixed vocabulary for Request::method: an
    // HTTP handler treats it as the HTTP verb, an FTP handler might
    // treat it as RETR/STOR/LIST, and a handler with no notion of a verb
    // at all is free to ignore it. NetworkManager itself never inspects
    // Request::method except when following a Redirect's own `method`.
    //
    // Threading and lifetime: start() must return quickly -- do the
    // actual I/O on the handler's own thread(s) and invoke `done`
    // asynchronously once a result is known, exactly once, from
    // whichever thread that ends up being. A handler is responsible for
    // honoring request.cancel (via CancellationToken::onCancel(), or by
    // polling CancellationToken::isCancelled() at natural checkpoints)
    // and for invoking `done` with a Cancelled NetError promptly once
    // cancellation fires, rather than leaving the caller waiting.
    //
    // Because a handler is normally owned by a dynamically loaded
    // module, and NetworkManager's HandlerHandle only weakly references
    // it (see NetworkManager's own docs), the handler's owning module
    // MUST ensure every start() call it has outstanding has already
    // invoked `done` before that module's onStop() returns -- otherwise
    // a call still in flight when the module's onShutdown()/destructor
    // runs (and, for a dynamically loaded module, when its library is
    // unmapped) would be left holding a `done` that points into code
    // that no longer exists. This mirrors the same obligation
    // dsbox_io::IOModule places on itself for its worker thread and
    // subprocess channels.
    class DSBOX_NET_API IProtocolHandler
    {
    public:
        virtual ~IProtocolHandler() = default;

        virtual void start(Request request, FetchCallback done) = 0;
    };
}
