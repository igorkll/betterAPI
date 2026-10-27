#pragma once

#include <dsbox_net_http/api.hpp>

#include <dsbox_net/protocol_handler.hpp>
#include <dsbox_net_socket/connection.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace dsbox_net_http
{
    // Finds the connector that opens connections for a URL scheme
    // ("http", "https"), or returns null if there is none. Looked up on
    // every request, so connectors may appear or disappear while the
    // handler lives. dsbox_net_http::HttpModule backs this with a
    // dsbox_net_socket::ConnectorRegistry.
    using ConnectorResolver = std::function<std::shared_ptr<dsbox_net_socket::IConnector>(std::string_view scheme)>;

    struct HttpOptions
    {
        // Sent when the request supplies no User-Agent of its own.
        std::string userAgent = "DSBox/0.1";

        // Reuse connections for further requests to the same
        // scheme/host/port once a response has been read completely.
        bool keepAlive = true;
        std::size_t maxIdleConnectionsPerHost = 4;
        std::size_t maxIdleConnections = 32;
        std::chrono::milliseconds idleConnectionTimeout{ 30000 };

        // Passed to the connector; zero means "the connector's default".
        std::chrono::milliseconds connectTimeout{ 0 };

        // Largest accepted status line + headers.
        std::size_t maxHeadBytes = 64 * 1024;

        // Buffering bound between the network and a slow body reader.
        std::size_t bodyPipeCapacity = 256 * 1024;
    };

    // HTTP/1.1 client protocol handler for dsbox_net::NetworkManager.
    //
    // Speaks HTTP/1.1 over whatever IConnection the resolved connector
    // returns -- plain TCP for http, a TLS connection for https. It
    // supports Content-Length, chunked and read-until-close bodies,
    // request bodies of known and unknown size, persistent connections,
    // and reports 301/302/303/307/308 as dsbox_net::Redirect so that
    // NetworkManager follows them (and re-runs its interceptors).
    //
    // Not supported (yet): response content codings (gzip, deflate, br
    // -- no Accept-Encoding is sent, so servers reply uncompressed),
    // HTTP/2, proxies, Expect: 100-continue, and protocol upgrades.
    //
    // Threading: every request runs on threads this handler owns. A
    // request uses one thread while it connects, sends and waits for the
    // response head; a second thread then streams the body into the
    // Response's BodyPipe, so a `done` callback that reads the body
    // synchronously cannot deadlock. After the head arrives, the
    // request's timeout no longer applies -- NetworkManager stops it when
    // `done` is called -- but cancelling the request's CancellationToken
    // still aborts the body transfer, failing the reader.
    //
    // Redirected requests that carry a body are replayed through
    // IBodySource::open(). Requests are retried once on a fresh
    // connection if a REUSED connection turns out to be dead before any
    // response byte arrived, but only for idempotent methods.
    class DSBOX_NET_HTTP_API HttpProtocolHandler final : public dsbox_net::IProtocolHandler
    {
    public:
        explicit HttpProtocolHandler(ConnectorResolver resolver, HttpOptions options = {});
        ~HttpProtocolHandler() override;

        HttpProtocolHandler(const HttpProtocolHandler&) = delete;
        HttpProtocolHandler& operator=(const HttpProtocolHandler&) = delete;

        void start(dsbox_net::Request request, dsbox_net::FetchCallback done) override;

        // Aborts every transfer in flight (each fails with Cancelled, or
        // its body reader fails), waits for all handler threads to
        // finish, and closes idle connections. After this, start() fails
        // immediately. Idempotent; the destructor calls it. Must not be
        // called from one of the handler's own threads (that is, from
        // inside a `done` callback).
        //
        // An in-progress DNS lookup cannot be interrupted, so this may
        // wait for it to return.
        void shutdown();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
