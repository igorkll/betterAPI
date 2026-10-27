#pragma once

#include <dsbox_net_socket/api.hpp>

#include <dsbox_net/cancellation.hpp>
#include <dsbox_net/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace dsbox_net_socket
{
    // A connected, bidirectional byte pipe to a peer -- a plain TCP
    // socket, a TLS session layered on one, or anything else that
    // behaves like them. Protocol-agnostic: knows nothing about HTTP (or
    // browsers), which is what lets other protocols reuse it.
    //
    // All I/O is BLOCKING. Callers run it on a thread they own.
    //
    // Threading: calls to read() and write() must not overlap -- one
    // thread at a time uses a connection for I/O (a TLS session, for
    // one, cannot be read and written from two threads at once). That
    // is all request/response protocols need. close() is the exception:
    // it is safe to call from ANY thread at ANY time, concurrently with
    // a read() or write() in progress, and is how a blocked I/O call is
    // aborted.
    class DSBOX_NET_SOCKET_API IConnection
    {
    public:
        virtual ~IConnection() = default;

        // Blocks until at least one byte is available, then copies up
        // to out.size() bytes. Returns 0 at end of stream (the peer
        // closed its side) and also after close() -- so a close() from
        // another thread makes a blocked read() return 0. Throws
        // dsbox_net::NetException if the connection fails.
        virtual std::size_t read(std::span<std::byte> out) = 0;

        // Writes ALL of `data` or throws dsbox_net::NetException.
        virtual void write(std::span<const std::byte> data) = 0;

        // Aborts the connection: wakes any blocked read()/write() (they
        // return 0 / throw) and makes all later I/O fail fast. Does not
        // release the underlying resource until the object is destroyed
        // -- that is what makes it safe to call from another thread
        // while I/O is in flight. Idempotent.
        virtual void close() noexcept = 0;
    };

    struct ConnectOptions
    {
        // Upper bound on name resolution + establishing the connection.
        // Zero means "the connector's own default".
        std::chrono::milliseconds timeout{ 0 };

        // Aborts the connect attempt when cancelled. A connector that
        // is blocked somewhere it cannot interrupt (a system DNS
        // lookup) notices at the next opportunity.
        dsbox_net::CancellationToken cancel;
    };

    // Opens connections to host:port. `host` is a DNS name or an IP
    // literal (an IPv6 literal may be written with or without []).
    //
    // Implementations are shared by many threads at once and so must be
    // thread-safe.
    class DSBOX_NET_SOCKET_API IConnector
    {
    public:
        virtual ~IConnector() = default;

        // Blocks until connected. Throws dsbox_net::NetException with
        // Dns / Connect / Timeout / Cancelled / Tls as appropriate.
        virtual std::shared_ptr<IConnection> connect(
            const std::string& host,
            std::uint16_t port,
            const ConnectOptions& options) = 0;
    };
}
