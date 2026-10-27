#pragma once

// Test-only helper: a tiny blocking TCP server on 127.0.0.1 with an
// ephemeral port, handing each accepted connection to a callback on its
// own thread. Shared by the socket and HTTP tests (the latter add this
// directory to their include path). Uses raw sockets deliberately, so the
// code under test is never also the thing playing the peer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

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
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace dsbox_test_net
{
#if defined(_WIN32)
    using RawSocket = SOCKET;
    inline constexpr RawSocket kBadSocket = INVALID_SOCKET;
    inline void closeRaw(RawSocket s) { ::closesocket(s); }
    inline void shutdownRaw(RawSocket s) { ::shutdown(s, SD_BOTH); }
    inline void initSockets()
    {
        static const bool once = [] { WSADATA d; return ::WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
        (void)once;
    }
#else
    using RawSocket = int;
    inline constexpr RawSocket kBadSocket = -1;
    inline void closeRaw(RawSocket s) { ::close(s); }
    inline void shutdownRaw(RawSocket s) { ::shutdown(s, SHUT_RDWR); }
    inline void initSockets() {}
#endif

    // One accepted connection, as seen from the server side.
    class RawConnection
    {
    public:
        explicit RawConnection(RawSocket socket) : m_socket(socket) {}
        ~RawConnection() { closeRaw(m_socket); }

        RawConnection(const RawConnection&) = delete;
        RawConnection& operator=(const RawConnection&) = delete;

        // Reads until `delimiter` has been seen (returned INCLUDING it)
        // or the peer closes (returns what arrived).
        std::string readUntil(std::string_view delimiter)
        {
            while (m_buffer.find(delimiter) == std::string::npos)
            {
                if (!fill())
                    break;
            }

            const auto pos = m_buffer.find(delimiter);
            const std::size_t end = pos == std::string::npos ? m_buffer.size() : pos + delimiter.size();
            std::string result = m_buffer.substr(0, end);
            m_buffer.erase(0, end);
            return result;
        }

        // Reads exactly `count` bytes (fewer only if the peer closes).
        std::string readExact(std::size_t count)
        {
            while (m_buffer.size() < count)
            {
                if (!fill())
                    break;
            }

            const std::size_t take = count < m_buffer.size() ? count : m_buffer.size();
            std::string result = m_buffer.substr(0, take);
            m_buffer.erase(0, take);
            return result;
        }

        // Blocks until the peer closes; returns everything it sent.
        std::string readToEnd()
        {
            while (fill())
            {
            }

            std::string result = std::move(m_buffer);
            m_buffer.clear();
            return result;
        }

        void send(std::string_view data)
        {
            while (!data.empty())
            {
                const auto sent = ::send(
                    m_socket,
                    data.data(),
#if defined(_WIN32)
                    static_cast<int>(data.size()),
#else
                    data.size(),
#endif
#if defined(MSG_NOSIGNAL)
                    MSG_NOSIGNAL
#else
                    0
#endif
                );

                if (sent <= 0)
                    return;

                data.remove_prefix(static_cast<std::size_t>(sent));
            }
        }

        // Closes the connection NOW (the destructor would too).
        void close() { shutdownRaw(m_socket); }

        // The underlying socket, for tests that layer something (TLS)
        // over it. Still owned by this object.
        RawSocket native() const { return m_socket; }

    private:
        bool fill()
        {
            char chunk[4096];
            const auto n = ::recv(m_socket, chunk,
#if defined(_WIN32)
                static_cast<int>(sizeof(chunk)),
#else
                sizeof(chunk),
#endif
                0);

            if (n <= 0)
                return false;

            m_buffer.append(chunk, static_cast<std::size_t>(n));
            return true;
        }

        RawSocket m_socket;
        std::string m_buffer;
    };

    class LoopbackServer
    {
    public:
        using Handler = std::function<void(RawConnection&)>;

        explicit LoopbackServer(Handler handler)
            : m_handler(std::move(handler))
        {
            initSockets();

            m_listener = ::socket(AF_INET, SOCK_STREAM, 0);

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = 0;

            int reuse = 1;
            ::setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

            ::bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
            ::listen(m_listener, 16);

#if defined(_WIN32)
            int length = sizeof(address);
#else
            socklen_t length = sizeof(address);
#endif
            ::getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &length);
            m_port = ntohs(address.sin_port);

            m_acceptThread = std::thread([this] { acceptLoop(); });
        }

        ~LoopbackServer()
        {
            m_stopping = true;

            if (m_acceptThread.joinable())
                m_acceptThread.join();

            closeRaw(m_listener);

            // A handler still blocked reading from a client that never
            // closed must not hang the test: shut its socket down.
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                for (RawSocket live : m_live)
                    shutdownRaw(live);
            }

            std::list<std::thread> threads;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                threads.swap(m_connectionThreads);
            }

            for (auto& thread : threads)
                thread.join();
        }

        LoopbackServer(const LoopbackServer&) = delete;
        LoopbackServer& operator=(const LoopbackServer&) = delete;

        std::uint16_t port() const { return m_port; }

        int connectionCount() const { return m_connections.load(); }

    private:
        void acceptLoop()
        {
            while (!m_stopping)
            {
#if defined(_WIN32)
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(m_listener, &readable);
                timeval timeout{ 0, 20000 };
                const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
#else
                pollfd descriptor{};
                descriptor.fd = m_listener;
                descriptor.events = POLLIN;
                const int ready = ::poll(&descriptor, 1, 20);
#endif
                if (ready <= 0)
                    continue;

                RawSocket client = ::accept(m_listener, nullptr, nullptr);

                if (client == kBadSocket)
                    continue;

                ++m_connections;

                std::lock_guard<std::mutex> lock(m_mutex);
                m_live.insert(client);
                m_connectionThreads.emplace_back([this, client]
                {
                    RawConnection connection(client);
                    m_handler(connection);

                    std::lock_guard<std::mutex> liveLock(m_mutex);
                    m_live.erase(client);
                });
            }
        }

        Handler m_handler;
        RawSocket m_listener = kBadSocket;
        std::uint16_t m_port = 0;
        std::atomic<bool> m_stopping{ false };
        std::atomic<int> m_connections{ 0 };
        std::thread m_acceptThread;
        std::mutex m_mutex;
        std::list<std::thread> m_connectionThreads;
        std::set<RawSocket> m_live;
    };
}
