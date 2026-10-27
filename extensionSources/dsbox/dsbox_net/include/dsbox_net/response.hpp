#pragma once

#include <dsbox_net/body.hpp>
#include <dsbox_net/error.hpp>
#include <dsbox_net/metadata.hpp>
#include <dsbox_net/url.hpp>

#include <memory>
#include <optional>
#include <string>

namespace dsbox_net
{
    // Response::body below is a std::unique_ptr<IByteStream>, and
    // unique_ptr's default deleter needs the complete type wherever a
    // Response's (or FetchResult's) destructor is implicitly generated
    // -- unlike IBodySource in request.hpp, which is only ever held by
    // shared_ptr (a type-erased deleter, safe to forward-declare). So
    // this header pulls in the whole of body.hpp rather than
    // forward-declaring IByteStream: every translation unit that touches
    // Response/FetchResult -- including ones that never call readAll()
    // or construct a BodyPipe themselves, like net_module.cpp -- needs
    // IByteStream complete for that destructor to compile.

    // Coarse, protocol-independent classification of a delivered
    // response's outcome -- deliberately not an HTTP status code
    // (see Response::status for that): a caller that doesn't care
    // which protocol answered can still tell "worked" from "not found"
    // from "denied" without a table of per-protocol codes.
    enum class StatusClass
    {
        Success,
        NotFound,
        Denied,      // authentication/authorization failure
        ClientError, // request was malformed/invalid in some other way
        ServerError, // the peer failed to fulfil an otherwise-valid request
        Other,
    };

    // What a handler returns from Response::redirect to have
    // NetworkManager follow a redirect on its behalf, instead of
    // handling it itself. `keepBody` matters only for a redirect that
    // preserves the original method (HTTP 307/308): true re-sends
    // Request::body on the new hop (re-opened via IBodySource::open()),
    // false drops it -- exactly the same fork HTTP's own 301/302/303
    // vs. 307/308 semantics require, just expressed generically so a
    // non-HTTP handler can use the same mechanism.
    struct Redirect
    {
        Url target;
        std::string method; // empty = keep the current request's method
        bool keepBody = false;
    };

    // What one fetch, or one hop of a redirected fetch, produced.
    // Constructing this and handing it to a FetchCallback is the ENTIRE
    // job of IProtocolHandler::start() -- see that class for the
    // contract in full.
    struct Response
    {
        Url url; // the URL this Response actually answers (see NetworkManager for the final one)

        // The protocol's own status code, unmodified -- an HTTP status
        // line's number, or whatever the handler's protocol uses. 0 if
        // the protocol has no such concept.
        int status = 0;
        StatusClass statusClass = StatusClass::Other;
        std::string statusText;

        Metadata metadata;

        // If set, NetworkManager follows this redirect (subject to
        // NetOptions::maxRedirects) instead of delivering this Response
        // to the caller; `body` below is then ignored/closed rather than
        // handed to anyone. A handler that has no notion of redirects
        // simply never sets this.
        std::optional<Redirect> redirect;

        // Absent (nullptr) for a response with no body (e.g. HEAD, or a
        // redirect -- see above). Owned by whoever receives this
        // Response; reading it is entirely the caller's responsibility,
        // NetworkManager never touches it.
        std::unique_ptr<IByteStream> body;
    };

    // The result of one fetch() call, as delivered to a FetchCallback:
    // either a delivery failure (`error` set, `response` otherwise
    // default/empty) or a genuinely delivered Response -- which, per
    // StatusClass's own docs, may still describe a protocol-level
    // failure like "not found" without `error` being set. Check ok()
    // first; only inspect `response` if it returns true.
    struct FetchResult
    {
        std::optional<NetError> error;
        Response response;

        bool ok() const noexcept
        {
            return !error.has_value();
        }
    };
}
