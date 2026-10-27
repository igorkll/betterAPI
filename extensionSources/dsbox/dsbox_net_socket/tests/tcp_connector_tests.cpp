#include "test_framework.hpp"
#include "loopback_server.hpp"

#include <dsbox_net_socket/tcp_connector.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace dsbox_net_socket_tests;
using namespace dsbox_net_socket;
using dsbox_test_net::LoopbackServer;
using dsbox_test_net::RawConnection;

namespace
{
    std::span<const std::byte> asBytes(const std::string& text)
    {
        return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
    }

    std::string readExactly(IConnection& connection, std::size_t count)
    {
        std::string result;
        std::vector<std::byte> buffer(4096);

        while (result.size() < count)
        {
            const std::size_t n = connection.read(buffer);

            if (n == 0)
                break;

            result.append(reinterpret_cast<const char*>(buffer.data()), n);
        }

        return result;
    }

    // Runs `action`; returns the NetErrorCode it threw, if any.
    template<typename F>
    std::optional<dsbox_net::NetErrorCode> errorCodeOf(F&& action)
    {
        try
        {
            action();
        }
        catch (const dsbox_net::NetException& exception)
        {
            return exception.error().code;
        }

        return std::nullopt;
    }

    void testConnectAndExchangeBytes()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            const std::string request = peer.readExact(5);
            peer.send("echo:" + request);
        });

        TcpConnector connector;
        auto connection = connector.connect("127.0.0.1", server.port(), {});

        const std::string hello = "hello";
        connection->write(asBytes(hello));

        DSBOX_NET_SOCKET_CHECK_EQ(readExactly(*connection, 10), "echo:hello");
    }

    void testReadReturnsZeroWhenPeerCloses()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.send("bye");
        });

        TcpConnector connector;
        auto connection = connector.connect("127.0.0.1", server.port(), {});

        DSBOX_NET_SOCKET_CHECK_EQ(readExactly(*connection, 100), "bye");

        std::byte buffer[8];
        DSBOX_NET_SOCKET_CHECK_EQ(connection->read(buffer), static_cast<std::size_t>(0));
    }

    void testLargeTransferArrivesIntact()
    {
        const std::size_t size = 3 * 1024 * 1024;

        LoopbackServer server([size](RawConnection& peer)
        {
            const std::string chunk(64 * 1024, 'x');

            for (std::size_t sent = 0; sent < size; sent += chunk.size())
                peer.send(chunk);
        });

        TcpConnector connector;
        auto connection = connector.connect("127.0.0.1", server.port(), {});

        const std::string received = readExactly(*connection, size);

        DSBOX_NET_SOCKET_CHECK_EQ(received.size(), size);
        DSBOX_NET_SOCKET_CHECK(received.find_first_not_of('x') == std::string::npos);
    }

    void testCloseFromAnotherThreadUnblocksRead()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readToEnd(); // never sends anything
        });

        TcpConnector connector;
        std::shared_ptr<IConnection> connection = connector.connect("127.0.0.1", server.port(), {});

        std::atomic<bool> returned{ false };
        std::size_t got = 99;

        std::thread reader([&]
        {
            std::byte buffer[8];
            got = connection->read(buffer);
            returned = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        DSBOX_NET_SOCKET_CHECK(!returned);

        connection->close();
        reader.join();

        DSBOX_NET_SOCKET_CHECK(returned);
        DSBOX_NET_SOCKET_CHECK_EQ(got, static_cast<std::size_t>(0));
    }

    void testWriteAfterCloseThrows()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readToEnd();
        });

        TcpConnector connector;
        auto connection = connector.connect("127.0.0.1", server.port(), {});
        connection->close();

        const std::string data = "x";
        const auto code = errorCodeOf([&] { connection->write(asBytes(data)); });

        DSBOX_NET_SOCKET_CHECK(code.has_value());
        DSBOX_NET_SOCKET_CHECK(code == dsbox_net::NetErrorCode::ConnectionReset);
    }

    void testConnectRefusedReportsConnectError()
    {
        std::uint16_t deadPort = 0;

        {
            LoopbackServer server([](RawConnection&) {});
            deadPort = server.port();
        } // listener closed: nothing accepts on deadPort any more

        TcpConnector connector;
        const auto code = errorCodeOf([&] { connector.connect("127.0.0.1", deadPort, {}); });

        DSBOX_NET_SOCKET_CHECK(code.has_value());
        DSBOX_NET_SOCKET_CHECK(code == dsbox_net::NetErrorCode::Connect);
    }

    void testAlreadyCancelledTokenFailsFast()
    {
        dsbox_net::CancellationSource source;
        source.cancel();

        ConnectOptions options;
        options.cancel = source.token();

        TcpConnector connector;
        const auto code = errorCodeOf([&] { connector.connect("127.0.0.1", 9, options); });

        DSBOX_NET_SOCKET_CHECK(code == dsbox_net::NetErrorCode::Cancelled);
    }

    void testCancelledReasonIsPropagated()
    {
        dsbox_net::CancellationSource source;
        source.cancel(dsbox_net::NetErrorCode::Timeout);

        ConnectOptions options;
        options.cancel = source.token();

        TcpConnector connector;
        const auto code = errorCodeOf([&] { connector.connect("127.0.0.1", 9, options); });

        DSBOX_NET_SOCKET_CHECK(code == dsbox_net::NetErrorCode::Timeout);
    }

    void testEmptyHostIsDnsError()
    {
        TcpConnector connector;
        const auto code = errorCodeOf([&] { connector.connect("", 80, {}); });

        DSBOX_NET_SOCKET_CHECK(code == dsbox_net::NetErrorCode::Dns);
    }

    void testUnresolvableHostIsDnsError()
    {
        // The ".invalid" TLD is reserved (RFC 2606) and never resolves.
        TcpConnector connector;
        const auto code = errorCodeOf([&] { connector.connect("dsbox-test.invalid", 80, {}); });

        DSBOX_NET_SOCKET_CHECK(code == dsbox_net::NetErrorCode::Dns);
    }

    void testBracketedIpv6LiteralIsAccepted()
    {
        // Whether the machine has IPv6 loopback varies; either it
        // connects or it reports Connect -- but a bracketed literal must
        // never be misreported as a DNS failure.
        TcpConnector connector;
        const auto code = errorCodeOf([&] { connector.connect("[::1]", 9, {}); });

        DSBOX_NET_SOCKET_CHECK(!code.has_value() || *code != dsbox_net::NetErrorCode::Dns);
    }
}

void runTcpConnectorTests()
{
    runTest("testConnectAndExchangeBytes", testConnectAndExchangeBytes);
    runTest("testReadReturnsZeroWhenPeerCloses", testReadReturnsZeroWhenPeerCloses);
    runTest("testLargeTransferArrivesIntact", testLargeTransferArrivesIntact);
    runTest("testCloseFromAnotherThreadUnblocksRead", testCloseFromAnotherThreadUnblocksRead);
    runTest("testWriteAfterCloseThrows", testWriteAfterCloseThrows);
    runTest("testConnectRefusedReportsConnectError", testConnectRefusedReportsConnectError);
    runTest("testAlreadyCancelledTokenFailsFast", testAlreadyCancelledTokenFailsFast);
    runTest("testCancelledReasonIsPropagated", testCancelledReasonIsPropagated);
    runTest("testEmptyHostIsDnsError", testEmptyHostIsDnsError);
    runTest("testUnresolvableHostIsDnsError", testUnresolvableHostIsDnsError);
    runTest("testBracketedIpv6LiteralIsAccepted", testBracketedIpv6LiteralIsAccepted);
}
