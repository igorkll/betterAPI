#pragma once

#include <dsbox_net/cancellation.hpp>
#include <dsbox_net/metadata.hpp>
#include <dsbox_net/url.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace dsbox_net
{
    class IBodySource;

    // One fetch request. Handed to NetworkManager::fetch(); flows
    // through every interceptor and (after any redirects) into the
    // IProtocolHandler chosen by url.scheme.
    struct Request
    {
        Url url;

        // The protocol-level verb/operation. HTTP: "GET"/"POST"/... .
        // FTP: something like "RETR"/"LIST" (this library imposes no
        // fixed vocabulary -- see IProtocolHandler's own docs on why).
        // Defaults to "GET" since that covers the overwhelmingly common
        // case (fetching a resource) across every protocol this targets.
        std::string method = "GET";

        Metadata metadata;

        // Absent (nullptr) means "no request body" -- appropriate for
        // GET/HEAD-shaped requests. See body.hpp for why this is a
        // re-openable source rather than a single-use stream: a
        // redirect that must resend the body (307/308) or a retry after
        // a transient failure needs to open() it again.
        std::shared_ptr<IBodySource> body;

        // Upper bound on the whole fetch, INCLUDING every redirect hop
        // -- not per-hop. 0 (the default) defers to
        // NetOptions::defaultTimeout.
        std::chrono::milliseconds timeout{ 0 };

        // Cooperative cancellation, checked by NetworkManager between
        // redirect hops and passed down to the protocol handler (which
        // is expected to honor it -- see IProtocolHandler). Defaults to
        // a token that can never fire, so a caller with nothing to
        // cancel doesn't need a CancellationSource just to fill this in.
        CancellationToken cancel;
    };
}
