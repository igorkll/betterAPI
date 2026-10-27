#include "test_framework.hpp"
#include "loopback_server.hpp"

#include <dsbox_net_http/http1.hpp>
#include <dsbox_net_http/http_protocol_handler.hpp>
#include <dsbox_net_socket/tcp_connector.hpp>

#include <dsbox_net/body.hpp>
#include <dsbox_net/cancellation.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace dsbox_net_http_tests;
using namespace dsbox_net_http;
using dsbox_net::FetchResult;
using dsbox_net::NetErrorCode;
using dsbox_net::Request;
using dsbox_test_net::LoopbackServer;
using dsbox_test_net::RawConnection;

namespace
{
    using namespace std::chrono_literals;

    /*
     * Helpers
     */

    std::shared_ptr<dsbox_net_socket::IConnector> sharedTcpConnector()
    {
        static const auto connector = std::make_shared<dsbox_net_socket::TcpConnector>();
        return connector;
    }

    std::unique_ptr<HttpProtocolHandler> makeHandler(HttpOptions options = {})
    {
        return std::make_unique<HttpProtocolHandler>(
            [](std::string_view) { return sharedTcpConnector(); },
            std::move(options));
    }

    Request makeRequest(std::uint16_t port, const std::string& pathAndQuery = "/", const std::string& method = "GET")
    {
        Request request;
        request.url = *dsbox_net::Url::parse("http://127.0.0.1:" + std::to_string(port) + pathAndQuery);
        request.method = method;
        return request;
    }

    // Runs one request and waits for the callback (or gives up).
    FetchResult fetchBlocking(HttpProtocolHandler& handler, Request request, std::chrono::milliseconds limit = 10s)
    {
        auto promise = std::make_shared<std::promise<FetchResult>>();
        auto future = promise->get_future();

        handler.start(std::move(request), [promise](FetchResult result)
        {
            promise->set_value(std::move(result));
        });

        if (future.wait_for(limit) != std::future_status::ready)
        {
            FetchResult timedOut;
            timedOut.error = dsbox_net::NetError{ NetErrorCode::Timeout, "test gave up waiting for the handler", 0 };
            return timedOut;
        }

        return future.get();
    }

    std::string bodyText(FetchResult& result)
    {
        if (!result.response.body)
            return "<no body>";

        const auto bytes = dsbox_net::readAll(*result.response.body, 64 * 1024 * 1024);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // Reads one request head from the peer; "" if the peer closed first.
    std::string readHead(RawConnection& peer)
    {
        const std::string head = peer.readUntil("\r\n\r\n");
        return head.find("\r\n\r\n") == std::string::npos ? std::string() : head;
    }

    std::string response200(const std::string& body, const std::string& extraHeaders = "")
    {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extraHeaders + "\r\n" + body;
    }

    // Runs `action`; returns the code of the NetException it threw.
    template<typename F>
    std::optional<NetErrorCode> errorCodeOf(F&& action)
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

    class StringByteStream final : public dsbox_net::IByteStream
    {
    public:
        StringByteStream(std::string text, std::size_t piece)
            : m_text(std::move(text)), m_piece(piece)
        {
        }

        std::size_t read(std::span<std::byte> out) override
        {
            const std::size_t n = std::min({ m_piece, m_text.size() - m_position, out.size() });
            std::memcpy(out.data(), m_text.data() + m_position, n);
            m_position += n;
            return n;
        }

        void close() noexcept override {}

    private:
        std::string m_text;
        std::size_t m_piece;
        std::size_t m_position = 0;
    };

    // A body whose size is not known up front, handed out in small
    // pieces -- forces chunked transfer coding.
    class UnsizedSource final : public dsbox_net::IBodySource
    {
    public:
        explicit UnsizedSource(std::string text) : m_text(std::move(text)) {}

        std::optional<std::uint64_t> size() const override { return std::nullopt; }

        std::unique_ptr<dsbox_net::IByteStream> open() override
        {
            return std::make_unique<StringByteStream>(m_text, 3);
        }

    private:
        std::string m_text;
    };

    std::shared_ptr<dsbox_net::IBodySource> textSource(const std::string& text)
    {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return dsbox_net::memorySource(std::move(bytes));
    }

    /*
     * Basic exchanges
     */

    void testGetWithContentLength()
    {
        std::string seenHead;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            peer.send(response200("hello world", "Content-Type: text/plain\r\n"));
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port(), "/a/b?x=1"));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.status, 200);
        DSBOX_NET_HTTP_CHECK(result.response.statusClass == dsbox_net::StatusClass::Success);
        DSBOX_NET_HTTP_CHECK_EQ(result.response.statusText, "OK");
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*result.response.metadata.get("content-type")), "text/plain");
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "hello world");

        DSBOX_NET_HTTP_CHECK(seenHead.rfind("GET /a/b?x=1 HTTP/1.1\r\n", 0) == 0);
        DSBOX_NET_HTTP_CHECK(seenHead.find("Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("User-Agent: DSBox/") != std::string::npos);
    }

    void testCallerHeadersAreSent()
    {
        std::string seenHead;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            peer.send(response200("x"));
        });

        auto request = makeRequest(server.port());
        request.metadata.set("X-Token", "abc123");
        request.metadata.set("User-Agent", "Custom/9");

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(seenHead.find("X-Token: abc123\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("User-Agent: Custom/9\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("DSBox/") == std::string::npos);
    }

    void testChunkedResponseBody()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                      "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(!result.response.metadata.has("Transfer-Encoding")); // undone by the handler
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "hello world");
    }

    void testBodyReadUntilClose()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nstreamed until close");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "streamed until close");
    }

    void testInterimResponseIsSkipped()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 100 Continue\r\n\r\n" + response200("final"));
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.status, 200);
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "final");
    }

    void testRepeatedResponseHeadersSurvive()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send(response200("", "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"));
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.metadata.getAll("set-cookie").size(), static_cast<std::size_t>(2));
    }

    void testStatusClassification()
    {
        const std::pair<int, dsbox_net::StatusClass> cases[] = {
            { 200, dsbox_net::StatusClass::Success },
            { 404, dsbox_net::StatusClass::NotFound },
            { 403, dsbox_net::StatusClass::Denied },
            { 400, dsbox_net::StatusClass::ClientError },
            { 503, dsbox_net::StatusClass::ServerError },
        };

        for (const auto& [status, expected] : cases)
        {
            LoopbackServer server([status = status](RawConnection& peer)
            {
                readHead(peer);
                peer.send("HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Length: 0\r\n\r\n");
            });

            auto handler = makeHandler();
            auto result = fetchBlocking(*handler, makeRequest(server.port()));

            DSBOX_NET_HTTP_CHECK(result.ok());
            DSBOX_NET_HTTP_CHECK_EQ(result.response.status, status);
            DSBOX_NET_HTTP_CHECK(result.response.statusClass == expected);
        }
    }

    void testHeadResponseHasNoBody()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"); // no body follows a HEAD
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port(), "/", "HEAD"));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(result.response.body == nullptr);
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*result.response.metadata.get("Content-Length")), "1000");
    }

    void testNoContentResponseHasEmptyBody()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 204 No Content\r\n\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.status, 204);
        DSBOX_NET_HTTP_CHECK(result.response.body != nullptr);
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "");
    }

    void testZeroLengthBodyIsAnEmptyStream()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send(response200(""));
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(result.response.body != nullptr);
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "");
    }

    /*
     * Request bodies
     */

    void testPostWithKnownSizeSendsContentLength()
    {
        std::string seenHead;
        std::string seenBody;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            seenBody = peer.readExact(11);
            peer.send(response200("ok"));
        });

        auto request = makeRequest(server.port(), "/submit", "POST");
        request.body = textSource("name=widget");

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(seenHead.find("Content-Length: 11\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("Transfer-Encoding") == std::string::npos);
        DSBOX_NET_HTTP_CHECK_EQ(seenBody, "name=widget");
    }

    void testPostWithoutBodySendsZeroContentLength()
    {
        std::string seenHead;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            peer.send(response200("ok"));
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port(), "/", "POST"));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(seenHead.find("Content-Length: 0\r\n") != std::string::npos);
    }

    void testGetWithoutBodySendsNoContentLength()
    {
        std::string seenHead;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            peer.send(response200("ok"));
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(seenHead.find("Content-Length") == std::string::npos);
    }

    void testBodyOfUnknownSizeIsSentChunked()
    {
        std::string seenHead;
        std::string seenBody;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            seenBody = peer.readUntil("0\r\n\r\n");
            peer.send(response200("ok"));
        });

        auto request = makeRequest(server.port(), "/upload", "POST");
        request.body = std::make_shared<UnsizedSource>("abcdefgh");

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(seenHead.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("Content-Length") == std::string::npos);
        DSBOX_NET_HTTP_CHECK_EQ(seenBody, "3\r\nabc\r\n3\r\ndef\r\n2\r\ngh\r\n0\r\n\r\n");
    }

    void testCallerCannotOverrideFraming()
    {
        std::string seenHead;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            peer.send(response200("ok"));
        });

        auto request = makeRequest(server.port());
        request.metadata.set("Host", "evil.example");
        request.metadata.set("Content-Length", "99999");
        request.metadata.set("Transfer-Encoding", "chunked");

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(seenHead.find("evil.example") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("99999") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(seenHead.find("Transfer-Encoding") == std::string::npos);
    }

    void testUnsafeRequestIsRejectedBeforeAnyIo()
    {
        LoopbackServer server([](RawConnection&) {});

        auto request = makeRequest(server.port());
        request.metadata.set("X-Bad", "v\r\nInjected: 1");

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::UnsupportedOperation);
    }

    /*
     * Redirects
     */

    void testRedirectIsReportedNotFollowed()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 302 Found\r\nLocation: /next?x=1\r\nContent-Length: 0\r\n\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port(), "/start"));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.status, 302);
        DSBOX_NET_HTTP_CHECK(result.response.redirect.has_value());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.redirect->target.path, "/next");
        DSBOX_NET_HTTP_CHECK_EQ(result.response.redirect->target.query, "x=1");
        DSBOX_NET_HTTP_CHECK_EQ(result.response.redirect->target.host, "127.0.0.1");
        DSBOX_NET_HTTP_CHECK_EQ(result.response.redirect->method, "");
        DSBOX_NET_HTTP_CHECK(result.response.redirect->keepBody);
    }

    void testRedirectMethodRules()
    {
        struct Case
        {
            int status;
            const char* method;
            const char* expectedMethod;
            bool expectedKeepBody;
        };

        const Case cases[] = {
            { 301, "POST", "GET", false },
            { 302, "POST", "GET", false },
            { 303, "POST", "GET", false },
            { 303, "PUT", "GET", false },
            { 303, "HEAD", "HEAD", false },
            { 307, "POST", "", true },
            { 308, "PUT", "", true },
            { 301, "GET", "", true },
        };

        for (const Case& c : cases)
        {
            LoopbackServer server([&c](RawConnection& peer)
            {
                readHead(peer);
                peer.send("HTTP/1.1 " + std::to_string(c.status) + " R\r\nLocation: /elsewhere\r\nContent-Length: 0\r\n\r\n");
            });

            auto handler = makeHandler();
            auto result = fetchBlocking(*handler, makeRequest(server.port(), "/", c.method));

            DSBOX_NET_HTTP_CHECK(result.ok());
            DSBOX_NET_HTTP_CHECK(result.response.redirect.has_value());

            if (result.response.redirect)
            {
                DSBOX_NET_HTTP_CHECK_EQ(result.response.redirect->method, std::string(c.expectedMethod));
                DSBOX_NET_HTTP_CHECK_EQ(result.response.redirect->keepBody, c.expectedKeepBody);
            }
        }
    }

    void testRedirectWithoutLocationIsDeliveredAsResponse()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 302 Found\r\nContent-Length: 4\r\n\r\nbody");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(!result.response.redirect.has_value());
        DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "body");
    }

    void testNotModifiedIsNotARedirect()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 304 Not Modified\r\nLocation: /x\r\n\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());
        DSBOX_NET_HTTP_CHECK(!result.response.redirect.has_value());
        DSBOX_NET_HTTP_CHECK_EQ(result.response.status, 304);
    }

    /*
     * Connection reuse
     */

    void testKeepAliveReusesConnection()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            for (;;)
            {
                if (readHead(peer).empty())
                    return;

                peer.send(response200("ok"));
            }
        });

        auto handler = makeHandler();

        for (int i = 0; i < 3; ++i)
        {
            auto result = fetchBlocking(*handler, makeRequest(server.port(), "/" + std::to_string(i)));

            DSBOX_NET_HTTP_CHECK(result.ok());
            DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "ok");

            // The connection goes back to the pool only after the body
            // thread has finished, a moment after the body was read.
            std::this_thread::sleep_for(100ms);
        }

        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 1);
    }

    void testKeepAliveReusesConnectionAfterChunkedBody()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            for (;;)
            {
                if (readHead(peer).empty())
                    return;

                peer.send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n");
            }
        });

        auto handler = makeHandler();

        for (int i = 0; i < 2; ++i)
        {
            auto result = fetchBlocking(*handler, makeRequest(server.port()));
            DSBOX_NET_HTTP_CHECK(result.ok());
            DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "ok");
            std::this_thread::sleep_for(100ms);
        }

        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 1);
    }

    void testKeepAliveReusesConnectionAfterRedirectAndNoContent()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            for (int n = 0;; ++n)
            {
                if (readHead(peer).empty())
                    return;

                if (n == 0)
                    peer.send("HTTP/1.1 302 Found\r\nLocation: /x\r\nContent-Length: 3\r\n\r\nabc");
                else
                    peer.send("HTTP/1.1 204 No Content\r\n\r\n");
            }
        });

        auto handler = makeHandler();

        auto first = fetchBlocking(*handler, makeRequest(server.port()));
        DSBOX_NET_HTTP_CHECK(first.ok());
        DSBOX_NET_HTTP_CHECK(first.response.redirect.has_value());

        auto second = fetchBlocking(*handler, makeRequest(server.port()));
        DSBOX_NET_HTTP_CHECK(second.ok());
        DSBOX_NET_HTTP_CHECK_EQ(second.response.status, 204);

        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 1);
    }

    void testConnectionCloseHeaderPreventsReuse()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send(response200("ok", "Connection: close\r\n"));
        });

        auto handler = makeHandler();

        for (int i = 0; i < 2; ++i)
        {
            auto result = fetchBlocking(*handler, makeRequest(server.port()));
            DSBOX_NET_HTTP_CHECK(result.ok());
            DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "ok");
            std::this_thread::sleep_for(100ms);
        }

        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 2);
    }

    void testKeepAliveCanBeDisabled()
    {
        std::string seenHead;

        LoopbackServer server([&](RawConnection& peer)
        {
            seenHead = readHead(peer);
            peer.send(response200("ok"));
        });

        HttpOptions options;
        options.keepAlive = false;

        auto handler = makeHandler(options);

        for (int i = 0; i < 2; ++i)
        {
            auto result = fetchBlocking(*handler, makeRequest(server.port()));
            DSBOX_NET_HTTP_CHECK(result.ok());
            bodyText(result);
            std::this_thread::sleep_for(100ms);
        }

        DSBOX_NET_HTTP_CHECK(seenHead.find("Connection: close\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 2);
    }

    void testStaleReusedConnectionIsRetriedForGet()
    {
        // Answers one request per connection, then closes it without
        // announcing that -- exactly what an idle timeout on a real
        // server looks like to a client holding a pooled connection.
        LoopbackServer server([](RawConnection& peer)
        {
            if (readHead(peer).empty())
                return;

            peer.send(response200("ok"));
            peer.close();
        });

        auto handler = makeHandler();

        for (int i = 0; i < 3; ++i)
        {
            auto result = fetchBlocking(*handler, makeRequest(server.port()));

            DSBOX_NET_HTTP_CHECK(result.ok());

            if (result.ok())
                DSBOX_NET_HTTP_CHECK_EQ(bodyText(result), "ok");

            std::this_thread::sleep_for(100ms);
        }

        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 3);
    }

    /*
     * Failures
     */

    void testConnectFailureIsReported()
    {
        std::uint16_t deadPort = 0;

        {
            LoopbackServer server([](RawConnection&) {});
            deadPort = server.port();
        }

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(deadPort));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Connect);
    }

    void testMalformedResponseIsProtocolError()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("this is not http\r\n\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Protocol);
    }

    void testConflictingContentLengthIsProtocolError()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Protocol);
    }

    void testCloseBeforeAnyResponseIsConnectionReset()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.close();
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::ConnectionReset);
    }

    void testOversizedHeadIsRejected()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nX-Big: " + std::string(100 * 1024, 'a') + "\r\n\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Protocol);
    }

    void testTruncatedBodyFailsTheReader()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nonly a little");
            peer.close();
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());

        const auto code = errorCodeOf([&] { bodyText(result); });

        DSBOX_NET_HTTP_CHECK(code.has_value());
        DSBOX_NET_HTTP_CHECK(code == NetErrorCode::ConnectionReset);
    }

    void testMalformedChunkedBodyFailsTheReader()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nbad\r\n");
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());

        const auto code = errorCodeOf([&] { bodyText(result); });

        DSBOX_NET_HTTP_CHECK(code == NetErrorCode::Protocol);
    }

    void testHttpsWithoutConnectorIsUnsupported()
    {
        HttpProtocolHandler handler([](std::string_view scheme) -> std::shared_ptr<dsbox_net_socket::IConnector>
        {
            return scheme == "http" ? sharedTcpConnector() : nullptr;
        });

        Request request;
        request.url = *dsbox_net::Url::parse("https://example.com/");

        auto result = fetchBlocking(handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::UnsupportedOperation);
    }

    void testForeignSchemeIsRejected()
    {
        auto handler = makeHandler();

        Request request;
        request.url = *dsbox_net::Url::parse("ftp://example.com/file");

        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::UnsupportedScheme);
    }

    void testMissingHostIsInvalidUrl()
    {
        auto handler = makeHandler();

        Request request;
        request.url.scheme = "http";
        request.url.path = "/";

        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::InvalidUrl);
    }

    /*
     * Cancellation and shutdown
     */

    void testCancelBeforeResponseHead()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.readToEnd(); // never answers
        });

        dsbox_net::CancellationSource source;
        auto request = makeRequest(server.port());
        request.cancel = source.token();

        auto handler = makeHandler();

        auto promise = std::make_shared<std::promise<FetchResult>>();
        auto future = promise->get_future();

        handler->start(std::move(request), [promise](FetchResult result)
        {
            promise->set_value(std::move(result));
        });

        std::this_thread::sleep_for(150ms);
        DSBOX_NET_HTTP_CHECK(future.wait_for(0ms) != std::future_status::ready);

        const auto cancelledAt = std::chrono::steady_clock::now();
        source.cancel();

        DSBOX_NET_HTTP_CHECK(future.wait_for(3s) == std::future_status::ready);

        const auto result = future.get();

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Cancelled);
        DSBOX_NET_HTTP_CHECK(std::chrono::steady_clock::now() - cancelledAt < 2s);
    }

    void testCancelReasonIsPropagated()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.readToEnd();
        });

        dsbox_net::CancellationSource source;
        auto request = makeRequest(server.port());
        request.cancel = source.token();

        auto handler = makeHandler();

        auto promise = std::make_shared<std::promise<FetchResult>>();
        auto future = promise->get_future();

        handler->start(std::move(request), [promise](FetchResult result)
        {
            promise->set_value(std::move(result));
        });

        std::this_thread::sleep_for(100ms);
        source.cancel(NetErrorCode::Timeout);

        DSBOX_NET_HTTP_CHECK(future.wait_for(3s) == std::future_status::ready);
        DSBOX_NET_HTTP_CHECK(future.get().error->code == NetErrorCode::Timeout);
    }

    void testAlreadyCancelledRequestFailsWithoutConnecting()
    {
        LoopbackServer server([](RawConnection&) {});

        dsbox_net::CancellationSource source;
        source.cancel();

        auto request = makeRequest(server.port());
        request.cancel = source.token();

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Cancelled);
        DSBOX_NET_HTTP_CHECK_EQ(server.connectionCount(), 0);
    }

    void testCancelDuringBodyFailsTheReader()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\nthe first few bytes");
            peer.readToEnd(); // then stalls until the client hangs up
        });

        dsbox_net::CancellationSource source;
        auto request = makeRequest(server.port());
        request.cancel = source.token();

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, std::move(request));

        DSBOX_NET_HTTP_CHECK(result.ok());

        std::this_thread::sleep_for(100ms);
        source.cancel();

        const auto code = errorCodeOf([&] { bodyText(result); });

        DSBOX_NET_HTTP_CHECK(code == NetErrorCode::Cancelled);
    }

    void testShutdownAbortsRequestsInFlight()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.readToEnd();
        });

        auto handler = makeHandler();

        auto promise = std::make_shared<std::promise<FetchResult>>();
        auto future = promise->get_future();

        handler->start(makeRequest(server.port()), [promise](FetchResult result)
        {
            promise->set_value(std::move(result));
        });

        std::this_thread::sleep_for(100ms);

        const auto before = std::chrono::steady_clock::now();
        handler->shutdown();

        DSBOX_NET_HTTP_CHECK(std::chrono::steady_clock::now() - before < 3s);
        DSBOX_NET_HTTP_CHECK(future.wait_for(0ms) == std::future_status::ready);

        const auto result = future.get();
        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Cancelled);
    }

    void testShutdownFailsTheBodyOfAStreamingResponse()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: 1000000\r\n\r\npartial");
            peer.readToEnd();
        });

        auto handler = makeHandler();
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());

        handler->shutdown();

        const auto code = errorCodeOf([&] { bodyText(result); });
        DSBOX_NET_HTTP_CHECK(code == NetErrorCode::Cancelled);
    }

    void testStartAfterShutdownFailsImmediately()
    {
        LoopbackServer server([](RawConnection&) {});

        auto handler = makeHandler();
        handler->shutdown();

        auto result = fetchBlocking(*handler, makeRequest(server.port()), 3s);

        DSBOX_NET_HTTP_CHECK(!result.ok());
        DSBOX_NET_HTTP_CHECK(result.error->code == NetErrorCode::Cancelled);
    }

    void testShutdownIsIdempotent()
    {
        auto handler = makeHandler();
        handler->shutdown();
        handler->shutdown();
    }

    /*
     * Streaming and back-pressure
     */

    void testLargeBodyThroughSmallPipe()
    {
        const std::size_t total = 4 * 1024 * 1024;

        LoopbackServer server([total](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n");

            const std::string block(32 * 1024, 'z');

            for (std::size_t sent = 0; sent < total; sent += block.size())
                peer.send(block);
        });

        HttpOptions options;
        options.bodyPipeCapacity = 64 * 1024; // far smaller than the body

        auto handler = makeHandler(options);
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());

        const std::string body = bodyText(result);

        DSBOX_NET_HTTP_CHECK_EQ(body.size(), total);
        DSBOX_NET_HTTP_CHECK(body.find_first_not_of('z') == std::string::npos);
    }

    void testDoneCallbackMayReadTheBodySynchronously()
    {
        // The callback runs on a handler thread and blocks reading a
        // body far larger than the pipe. That only works because the
        // body is produced on a different thread than the callback.
        const std::size_t total = 2 * 1024 * 1024;

        LoopbackServer server([total](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n");

            const std::string block(32 * 1024, 'q');

            for (std::size_t sent = 0; sent < total; sent += block.size())
                peer.send(block);
        });

        HttpOptions options;
        options.bodyPipeCapacity = 32 * 1024;

        auto handler = makeHandler(options);

        auto promise = std::make_shared<std::promise<std::size_t>>();
        auto future = promise->get_future();

        handler->start(makeRequest(server.port()), [promise](FetchResult result)
        {
            std::size_t size = 0;

            try
            {
                size = dsbox_net::readAll(*result.response.body, 64 * 1024 * 1024).size();
            }
            catch (...)
            {
            }

            promise->set_value(size);
        });

        DSBOX_NET_HTTP_CHECK(future.wait_for(15s) == std::future_status::ready);

        if (future.wait_for(0ms) == std::future_status::ready)
            DSBOX_NET_HTTP_CHECK_EQ(future.get(), total);
    }

    void testClosingTheReaderEarlyReleasesTheConnection()
    {
        std::atomic<bool> serverNoticedClose{ false };

        LoopbackServer server([&](RawConnection& peer)
        {
            readHead(peer);
            peer.send("HTTP/1.1 200 OK\r\nContent-Length: 100000000\r\n\r\n");

            const std::string block(64 * 1024, 'x');

            // send() returns without doing anything once the client
            // has hung up; keep going until the connection is gone.
            for (int i = 0; i < 100000; ++i)
                peer.send(block);

            serverNoticedClose = true;
        });

        HttpOptions options;
        options.bodyPipeCapacity = 64 * 1024;

        auto handler = makeHandler(options);
        auto result = fetchBlocking(*handler, makeRequest(server.port()));

        DSBOX_NET_HTTP_CHECK(result.ok());

        std::byte buffer[16];
        DSBOX_NET_HTTP_CHECK(result.response.body->read(buffer) > 0);
        result.response.body->close(); // reader walks away from a 100 MB body

        for (int i = 0; i < 100 && !serverNoticedClose; ++i)
            std::this_thread::sleep_for(50ms);

        DSBOX_NET_HTTP_CHECK(serverNoticedClose);
    }

    void testConcurrentRequests()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            for (;;)
            {
                const std::string head = readHead(peer);

                if (head.empty())
                    return;

                const auto start = head.find(' ') + 1;
                const auto end = head.find(' ', start);
                peer.send(response200("echo:" + head.substr(start, end - start)));
            }
        });

        auto handler = makeHandler();

        std::atomic<int> failures{ 0 };
        std::vector<std::thread> threads;

        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back([&, t]
            {
                for (int i = 0; i < 10; ++i)
                {
                    const std::string path = "/t" + std::to_string(t) + "/" + std::to_string(i);
                    auto result = fetchBlocking(*handler, makeRequest(server.port(), path));

                    if (!result.ok() || bodyText(result) != "echo:" + path)
                        ++failures;
                }
            });
        }

        for (auto& thread : threads)
            thread.join();

        DSBOX_NET_HTTP_CHECK_EQ(failures.load(), 0);
    }
}

void runHttpHandlerTests()
{
    runTest("testGetWithContentLength", testGetWithContentLength);
    runTest("testCallerHeadersAreSent", testCallerHeadersAreSent);
    runTest("testChunkedResponseBody", testChunkedResponseBody);
    runTest("testBodyReadUntilClose", testBodyReadUntilClose);
    runTest("testInterimResponseIsSkipped", testInterimResponseIsSkipped);
    runTest("testRepeatedResponseHeadersSurvive", testRepeatedResponseHeadersSurvive);
    runTest("testStatusClassification", testStatusClassification);
    runTest("testHeadResponseHasNoBody", testHeadResponseHasNoBody);
    runTest("testNoContentResponseHasEmptyBody", testNoContentResponseHasEmptyBody);
    runTest("testZeroLengthBodyIsAnEmptyStream", testZeroLengthBodyIsAnEmptyStream);

    runTest("testPostWithKnownSizeSendsContentLength", testPostWithKnownSizeSendsContentLength);
    runTest("testPostWithoutBodySendsZeroContentLength", testPostWithoutBodySendsZeroContentLength);
    runTest("testGetWithoutBodySendsNoContentLength", testGetWithoutBodySendsNoContentLength);
    runTest("testBodyOfUnknownSizeIsSentChunked", testBodyOfUnknownSizeIsSentChunked);
    runTest("testCallerCannotOverrideFraming", testCallerCannotOverrideFraming);
    runTest("testUnsafeRequestIsRejectedBeforeAnyIo", testUnsafeRequestIsRejectedBeforeAnyIo);

    runTest("testRedirectIsReportedNotFollowed", testRedirectIsReportedNotFollowed);
    runTest("testRedirectMethodRules", testRedirectMethodRules);
    runTest("testRedirectWithoutLocationIsDeliveredAsResponse", testRedirectWithoutLocationIsDeliveredAsResponse);
    runTest("testNotModifiedIsNotARedirect", testNotModifiedIsNotARedirect);

    runTest("testKeepAliveReusesConnection", testKeepAliveReusesConnection);
    runTest("testKeepAliveReusesConnectionAfterChunkedBody", testKeepAliveReusesConnectionAfterChunkedBody);
    runTest("testKeepAliveReusesConnectionAfterRedirectAndNoContent", testKeepAliveReusesConnectionAfterRedirectAndNoContent);
    runTest("testConnectionCloseHeaderPreventsReuse", testConnectionCloseHeaderPreventsReuse);
    runTest("testKeepAliveCanBeDisabled", testKeepAliveCanBeDisabled);
    runTest("testStaleReusedConnectionIsRetriedForGet", testStaleReusedConnectionIsRetriedForGet);

    runTest("testConnectFailureIsReported", testConnectFailureIsReported);
    runTest("testMalformedResponseIsProtocolError", testMalformedResponseIsProtocolError);
    runTest("testConflictingContentLengthIsProtocolError", testConflictingContentLengthIsProtocolError);
    runTest("testCloseBeforeAnyResponseIsConnectionReset", testCloseBeforeAnyResponseIsConnectionReset);
    runTest("testOversizedHeadIsRejected", testOversizedHeadIsRejected);
    runTest("testTruncatedBodyFailsTheReader", testTruncatedBodyFailsTheReader);
    runTest("testMalformedChunkedBodyFailsTheReader", testMalformedChunkedBodyFailsTheReader);
    runTest("testHttpsWithoutConnectorIsUnsupported", testHttpsWithoutConnectorIsUnsupported);
    runTest("testForeignSchemeIsRejected", testForeignSchemeIsRejected);
    runTest("testMissingHostIsInvalidUrl", testMissingHostIsInvalidUrl);

    runTest("testCancelBeforeResponseHead", testCancelBeforeResponseHead);
    runTest("testCancelReasonIsPropagated", testCancelReasonIsPropagated);
    runTest("testAlreadyCancelledRequestFailsWithoutConnecting", testAlreadyCancelledRequestFailsWithoutConnecting);
    runTest("testCancelDuringBodyFailsTheReader", testCancelDuringBodyFailsTheReader);
    runTest("testShutdownAbortsRequestsInFlight", testShutdownAbortsRequestsInFlight);
    runTest("testShutdownFailsTheBodyOfAStreamingResponse", testShutdownFailsTheBodyOfAStreamingResponse);
    runTest("testStartAfterShutdownFailsImmediately", testStartAfterShutdownFailsImmediately);
    runTest("testShutdownIsIdempotent", testShutdownIsIdempotent);

    runTest("testLargeBodyThroughSmallPipe", testLargeBodyThroughSmallPipe);
    runTest("testDoneCallbackMayReadTheBodySynchronously", testDoneCallbackMayReadTheBodySynchronously);
    runTest("testClosingTheReaderEarlyReleasesTheConnection", testClosingTheReaderEarlyReleasesTheConnection);
    runTest("testConcurrentRequests", testConcurrentRequests);
}
