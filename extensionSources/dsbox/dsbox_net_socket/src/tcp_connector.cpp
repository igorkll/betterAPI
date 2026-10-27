#include <dsbox_net_socket/tcp_connector.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace dsbox_net_socket
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

#if defined(_WIN32)
        using NativeSocket = SOCKET;
        using SockLen = int;
        constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
        constexpr int kShutdownBoth = SD_BOTH;
        constexpr int kSendFlags = 0;

        int lastSocketError() noexcept
        {
            return ::WSAGetLastError();
        }

        void closeSocket(NativeSocket socket) noexcept
        {
            ::closesocket(socket);
        }

        bool isInterrupted(int error) noexcept
        {
            return error == WSAEINTR;
        }

        bool isInProgress(int error) noexcept
        {
            return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
        }

        void setBlocking(NativeSocket socket, bool blocking) noexcept
        {
            u_long mode = blocking ? 0 : 1;
            ::ioctlsocket(socket, FIONBIO, &mode);
        }

        void ensureNetworkingInitialized()
        {
            // Winsock must be initialised once per process before any
            // other call. Deliberately never WSACleanup()'d: sockets may
            // outlive any single connector, and the OS reclaims it at
            // process exit.
            static const bool initialised = []
            {
                WSADATA data;
                return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();

            (void)initialised;
        }
#else
        using NativeSocket = int;
        using SockLen = socklen_t;
        constexpr NativeSocket kInvalidSocket = -1;
        constexpr int kShutdownBoth = SHUT_RDWR;
    #if defined(MSG_NOSIGNAL)
        constexpr int kSendFlags = MSG_NOSIGNAL; // a dead peer must be an error, not SIGPIPE
    #else
        constexpr int kSendFlags = 0;
    #endif

        int lastSocketError() noexcept
        {
            return errno;
        }

        void closeSocket(NativeSocket socket) noexcept
        {
            ::close(socket);
        }

        bool isInterrupted(int error) noexcept
        {
            return error == EINTR;
        }

        bool isInProgress(int error) noexcept
        {
            return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN;
        }

        void setBlocking(NativeSocket socket, bool blocking) noexcept
        {
            const int flags = ::fcntl(socket, F_GETFL, 0);
            if (flags < 0)
                return;

            ::fcntl(socket, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
        }

        void ensureNetworkingInitialized()
        {
        }
#endif

        std::string describeError(int error)
        {
            return std::system_category().message(error);
        }

        [[noreturn]] void fail(dsbox_net::NetErrorCode code, std::string message, int nativeCode = 0)
        {
            throw dsbox_net::NetException(dsbox_net::NetError{ code, std::move(message), nativeCode });
        }

        void setIntOption(NativeSocket socket, int level, int name, int value) noexcept
        {
            ::setsockopt(socket, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
        }

        // One established TCP connection. close() only shutdown()s the
        // socket -- the descriptor itself is released in the destructor.
        // That asymmetry is the whole point: closing a descriptor that
        // another thread is blocked in recv() on is a race (the number
        // can be reused), whereas shutdown() safely wakes that thread.
        class TcpConnection final : public IConnection
        {
        public:
            explicit TcpConnection(NativeSocket socket) noexcept
                : m_socket(socket)
            {
            }

            ~TcpConnection() override
            {
                close();
                closeSocket(m_socket);
            }

            std::size_t read(std::span<std::byte> out) override
            {
                if (out.empty() || m_closed.load())
                    return 0;

                const std::size_t want = std::min<std::size_t>(out.size(), 1u << 20);

                for (;;)
                {
                    const auto received = ::recv(
                        m_socket,
                        reinterpret_cast<char*>(out.data()),
#if defined(_WIN32)
                        static_cast<int>(want),
#else
                        want,
#endif
                        0);

                    if (received > 0)
                        return static_cast<std::size_t>(received);

                    if (received == 0)
                        return 0;

                    const int error = lastSocketError();

                    if (isInterrupted(error))
                        continue;

                    if (m_closed.load())
                        return 0; // our own close() woke us

                    fail(dsbox_net::NetErrorCode::ConnectionReset, "read failed: " + describeError(error), error);
                }
            }

            void write(std::span<const std::byte> data) override
            {
                while (!data.empty())
                {
                    if (m_closed.load())
                        fail(dsbox_net::NetErrorCode::ConnectionReset, "connection closed");

                    const std::size_t chunk = std::min<std::size_t>(data.size(), 1u << 20);

                    const auto sent = ::send(
                        m_socket,
                        reinterpret_cast<const char*>(data.data()),
#if defined(_WIN32)
                        static_cast<int>(chunk),
#else
                        chunk,
#endif
                        kSendFlags);

                    if (sent > 0)
                    {
                        data = data.subspan(static_cast<std::size_t>(sent));
                        continue;
                    }

                    const int error = lastSocketError();

                    if (sent < 0 && isInterrupted(error))
                        continue;

                    fail(dsbox_net::NetErrorCode::ConnectionReset, "write failed: " + describeError(error), error);
                }
            }

            void close() noexcept override
            {
                if (!m_closed.exchange(true))
                    ::shutdown(m_socket, kShutdownBoth);
            }

        private:
            NativeSocket m_socket;
            std::atomic<bool> m_closed{ false };
        };

        enum class Attempt
        {
            Connected,
            Failed,
            TimedOut,
            Cancelled,
        };

        // Tries ONE address. On Connected, `socket` is a connected,
        // blocking socket; otherwise it is closed and `error` holds the
        // OS error for Failed.
        Attempt attemptConnect(
            const addrinfo& address,
            Clock::time_point deadline,
            const dsbox_net::CancellationToken& cancel,
            NativeSocket& socket,
            int& error)
        {
            socket = ::socket(address.ai_family, address.ai_socktype, address.ai_protocol);

            if (socket == kInvalidSocket)
            {
                error = lastSocketError();
                return Attempt::Failed;
            }

            setBlocking(socket, false);

            const int rc = ::connect(socket, address.ai_addr, static_cast<SockLen>(address.ai_addrlen));

            if (rc != 0)
            {
                error = lastSocketError();

                if (!isInProgress(error))
                {
                    closeSocket(socket);
                    return Attempt::Failed;
                }

                // Connection in progress: wait in short slices so a
                // cancel is noticed promptly.
                for (;;)
                {
                    if (cancel.isCancelled())
                    {
                        closeSocket(socket);
                        return Attempt::Cancelled;
                    }

                    const auto now = Clock::now();

                    if (now >= deadline)
                    {
                        closeSocket(socket);
                        return Attempt::TimedOut;
                    }

                    const auto slice = std::min<Clock::duration>(
                        deadline - now,
                        std::chrono::milliseconds(50));
                    const auto sliceMs = std::max<long long>(
                        1,
                        std::chrono::duration_cast<std::chrono::milliseconds>(slice).count());

                    int ready = 0;

#if defined(_WIN32)
                    fd_set writable;
                    fd_set failed;
                    FD_ZERO(&writable);
                    FD_ZERO(&failed);
                    FD_SET(socket, &writable);
                    FD_SET(socket, &failed);

                    timeval timeout{};
                    timeout.tv_sec = static_cast<long>(sliceMs / 1000);
                    timeout.tv_usec = static_cast<long>((sliceMs % 1000) * 1000);

                    ready = ::select(0, nullptr, &writable, &failed, &timeout);
#else
                    pollfd descriptor{};
                    descriptor.fd = socket;
                    descriptor.events = POLLOUT;

                    ready = ::poll(&descriptor, 1, static_cast<int>(sliceMs));
#endif

                    if (ready < 0)
                    {
                        const int pollError = lastSocketError();

                        if (isInterrupted(pollError))
                            continue;

                        error = pollError;
                        closeSocket(socket);
                        return Attempt::Failed;
                    }

                    if (ready == 0)
                        continue;

                    int pending = 0;
                    SockLen length = sizeof(pending);

                    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&pending), &length) != 0)
                        pending = lastSocketError();

                    if (pending != 0)
                    {
                        error = pending;
                        closeSocket(socket);
                        return Attempt::Failed;
                    }

                    break; // connected
                }
            }

            setBlocking(socket, true);
            return Attempt::Connected;
        }

        std::string stripBrackets(const std::string& host)
        {
            if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
                return host.substr(1, host.size() - 2);

            return host;
        }

        struct AddrInfoDeleter
        {
            void operator()(addrinfo* info) const noexcept
            {
                if (info)
                    ::freeaddrinfo(info);
            }
        };
    }

    TcpConnector::TcpConnector() = default;

    TcpConnector::TcpConnector(TcpConnectorOptions options)
        : m_options(options)
    {
    }

    TcpConnector::~TcpConnector() = default;

    std::shared_ptr<IConnection> TcpConnector::connect(
        const std::string& host,
        std::uint16_t port,
        const ConnectOptions& options)
    {
        ensureNetworkingInitialized();

        const auto throwIfCancelled = [&]
        {
            if (options.cancel.isCancelled())
                fail(options.cancel.reason(), "connect cancelled");
        };

        throwIfCancelled();

        const std::string hostName = stripBrackets(host);

        if (hostName.empty())
            fail(dsbox_net::NetErrorCode::Dns, "empty host name");

        const auto timeout = options.timeout.count() > 0 ? options.timeout : m_options.defaultTimeout;
        const auto deadline = timeout.count() > 0 ? Clock::now() + timeout : Clock::time_point::max();

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        const std::string portText = std::to_string(port);
        addrinfo* raw = nullptr;

        const int resolved = ::getaddrinfo(hostName.c_str(), portText.c_str(), &hints, &raw);
        std::unique_ptr<addrinfo, AddrInfoDeleter> addresses(raw);

        if (resolved != 0 || !addresses)
        {
#if defined(_WIN32)
            fail(dsbox_net::NetErrorCode::Dns, "could not resolve '" + hostName + "': " + describeError(resolved), resolved);
#else
            fail(dsbox_net::NetErrorCode::Dns, "could not resolve '" + hostName + "': " + ::gai_strerror(resolved), resolved);
#endif
        }

        int lastError = 0;
        bool lastWasTimeout = false;

        for (const addrinfo* address = addresses.get(); address != nullptr; address = address->ai_next)
        {
            throwIfCancelled();

            const auto now = Clock::now();

            if (now >= deadline)
            {
                lastWasTimeout = true;
                break;
            }

            auto attemptDeadline = deadline;

            if (m_options.perAddressTimeout.count() > 0)
                attemptDeadline = std::min(attemptDeadline, now + m_options.perAddressTimeout);

            NativeSocket socket = kInvalidSocket;
            int error = 0;

            switch (attemptConnect(*address, attemptDeadline, options.cancel, socket, error))
            {
            case Attempt::Connected:
                setIntOption(socket, SOL_SOCKET, SO_KEEPALIVE, m_options.keepAlive ? 1 : 0);

                if (m_options.noDelay)
                    setIntOption(socket, IPPROTO_TCP, TCP_NODELAY, 1);

#if defined(SO_NOSIGPIPE)
                setIntOption(socket, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
                return std::make_shared<TcpConnection>(socket);

            case Attempt::Cancelled:
                fail(options.cancel.reason(), "connect cancelled");

            case Attempt::TimedOut:
                lastWasTimeout = true;
                lastError = 0;
                break;

            case Attempt::Failed:
                lastWasTimeout = false;
                lastError = error;
                break;
            }
        }

        if (lastWasTimeout)
            fail(dsbox_net::NetErrorCode::Timeout, "timed out connecting to " + hostName + ":" + portText);

        fail(
            dsbox_net::NetErrorCode::Connect,
            "could not connect to " + hostName + ":" + portText + ": " + describeError(lastError),
            lastError);
    }
}
