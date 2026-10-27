#pragma once

#include <dsbox_net/api.hpp>

#include <stdexcept>
#include <string>

namespace dsbox_net
{
    // Coarse classification of why a fetch failed to deliver a Response
    // at all. Deliberately does NOT include HTTP-style status like
    // "not found" or "forbidden" -- those are successful deliveries as
    // far as this enum is concerned (see Response::statusClass); this is
    // only for failures where no meaningful Response body exists.
    enum class NetErrorCode
    {
        InvalidUrl,          // Request::url (or a resolved redirect target) doesn't parse
        UnsupportedScheme,   // no IProtocolHandler registered for the URL's scheme
        UnsupportedOperation,// the handler understood the scheme but not this request shape
        Dns,
        Connect,
        ConnectionReset,
        Timeout,
        Tls,
        Protocol,            // malformed response from the peer
        TooManyRedirects,
        Cancelled,
        Internal,            // NetworkManager/interceptor-side bug, not the peer's fault
    };

    // Human-readable name for `code`, e.g. "Timeout" -- for logging, not
    // meant to be shown to end users as-is.
    DSBOX_NET_API const char* toString(NetErrorCode code) noexcept;

    // Describes one fetch failure. `nativeCode`, if non-zero, is
    // whatever OS/library-specific error number the handler that
    // produced this happened to have on hand (errno, an OS socket error,
    // a TLS library's own code, ...) -- informational only, callers
    // should switch on `code`, never on `nativeCode`.
    struct NetError
    {
        NetErrorCode code = NetErrorCode::Internal;
        std::string message;
        int nativeCode = 0;
    };

    // Thrown by the blocking IByteStream::read() overloads (see
    // body.hpp) when the underlying transfer fails mid-stream. Not used
    // for delivery failures reported through FetchResult -- those never
    // throw, they report `error` in the result instead (see
    // request.hpp/response.hpp) -- this is only for a failure that
    // happens after headers were already delivered and the caller is
    // now pulling the body.
    class DSBOX_NET_API NetException : public std::runtime_error
    {
    public:
        explicit NetException(NetError error)
            : std::runtime_error(error.message)
            , m_error(std::move(error))
        {
        }

        const NetError& error() const noexcept
        {
            return m_error;
        }

    private:
        NetError m_error;
    };
}
