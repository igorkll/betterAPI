#include "test_framework.hpp"
#include "test_pki.hpp"

#include <dsbox_net_http/http_protocol_handler.hpp>
#include <dsbox_net_socket/tcp_connector.hpp>
#include <dsbox_net_tls/trust_manager.hpp>
#include <dsbox_net_tls_openssl/tls_connector.hpp>

#include <dsbox_net/body.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The whole client stack, bottom to top: HttpProtocolHandler ->
// TlsConnector -> TcpConnector, against a real OpenSSL server.

using namespace dsbox_net_tls_openssl_tests;
using namespace dsbox_test_pki;
using dsbox_net::NetErrorCode;
using dsbox_test_net::LoopbackServer;
using dsbox_test_net::RawConnection;

namespace
{
    using namespace std::chrono_literals;

    struct Pki
    {
        Identity ca;
        Identity server;
    };

    Pki makePki()
    {
        CertSpec caSpec;
        caSpec.commonName = "DSBox Test CA";
        caSpec.isCa = true;

        CertSpec serverSpec;
        serverSpec.commonName = "localhost";
        serverSpec.alternativeNames = "DNS:localhost,IP:127.0.0.1";

        Pki pki;
        pki.ca = issue(caSpec);
        pki.server = issue(serverSpec, &pki.ca);
        return pki;
    }

    struct Stack
    {
        std::shared_ptr<dsbox_net_tls::TrustManager> trust = std::make_shared<dsbox_net_tls::TrustManager>();
        std::shared_ptr<FixedTrust> provider = std::make_shared<FixedTrust>();
        std::shared_ptr<dsbox_net_socket::IConnector> tcp = std::make_shared<dsbox_net_socket::TcpConnector>();
        std::shared_ptr<dsbox_net_socket::IConnector> tls;
        std::unique_ptr<dsbox_net_http::HttpProtocolHandler> handler;

        explicit Stack(std::vector<dsbox_net_tls::Certificate> trusted)
        {
            provider->set(std::move(trusted));
            trust->addProvider(provider);
            tls = std::make_shared<dsbox_net_tls_openssl::TlsConnector>(tcp, trust);

            auto plain = tcp;
            auto secure = tls;

            handler = std::make_unique<dsbox_net_http::HttpProtocolHandler>(
                [plain, secure](std::string_view scheme) -> std::shared_ptr<dsbox_net_socket::IConnector>
                {
                    if (scheme == "https")
                        return secure;

                    if (scheme == "http")
                        return plain;

                    return nullptr;
                });
        }
    };

    dsbox_net::FetchResult fetch(dsbox_net_http::HttpProtocolHandler& handler, const std::string& url)
    {
        dsbox_net::Request request;
        request.url = *dsbox_net::Url::parse(url);

        auto promise = std::make_shared<std::promise<dsbox_net::FetchResult>>();
        auto future = promise->get_future();

        handler.start(std::move(request), [promise](dsbox_net::FetchResult result)
        {
            promise->set_value(std::move(result));
        });

        if (future.wait_for(15s) != std::future_status::ready)
        {
            dsbox_net::FetchResult timedOut;
            timedOut.error = dsbox_net::NetError{ NetErrorCode::Timeout, "test gave up waiting", 0 };
            return timedOut;
        }

        return future.get();
    }

    std::string bodyOf(dsbox_net::FetchResult& result)
    {
        const auto bytes = dsbox_net::readAll(*result.response.body, 64 * 1024 * 1024);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // An HTTPS server that answers every request on a connection with
    // "secure:<path>" until the client goes away.
    std::unique_ptr<LoopbackServer> httpsEchoServer(const Pki& pki)
    {
        auto context = makeServerContext(pki.server);

        return std::make_unique<LoopbackServer>([context](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);

            if (!tls.handshake())
                return;

            for (;;)
            {
                const std::string head = tls.readUntil("\r\n\r\n");

                if (head.find("\r\n\r\n") == std::string::npos)
                    return;

                const auto start = head.find(' ') + 1;
                const auto end = head.find(' ', start);
                const std::string body = "secure:" + head.substr(start, end - start);

                if (!tls.send("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body))
                    return;
            }
        });
    }

    void testHttpsGetThroughTheWholeStack()
    {
        const Pki pki = makePki();
        auto server = httpsEchoServer(pki);

        Stack stack({ pemCertificate(pki.ca.cert.get()) });

        auto result = fetch(*stack.handler, "https://localhost:" + std::to_string(server->port()) + "/hello?x=1");

        DSBOX_NET_TLS_CHECK(result.ok());

        if (!result.ok())
            return;

        DSBOX_NET_TLS_CHECK_EQ(result.response.status, 200);
        DSBOX_NET_TLS_CHECK_EQ(bodyOf(result), "secure:/hello?x=1");
    }

    void testHttpsConnectionIsReusedAcrossRequests()
    {
        const Pki pki = makePki();
        auto server = httpsEchoServer(pki);

        Stack stack({ pemCertificate(pki.ca.cert.get()) });

        for (int i = 0; i < 3; ++i)
        {
            auto result = fetch(*stack.handler, "https://localhost:" + std::to_string(server->port()) + "/r" + std::to_string(i));

            DSBOX_NET_TLS_CHECK(result.ok());

            if (result.ok())
                DSBOX_NET_TLS_CHECK_EQ(bodyOf(result), "secure:/r" + std::to_string(i));

            // The connection returns to the pool a moment after the
            // body has been read.
            std::this_thread::sleep_for(100ms);
        }

        DSBOX_NET_TLS_CHECK_EQ(server->connectionCount(), 1);
    }

    void testHttpsToUntrustedServerReportsATlsError()
    {
        const Pki pki = makePki();
        auto server = httpsEchoServer(pki);

        Stack stack({}); // trusts nobody

        auto result = fetch(*stack.handler, "https://localhost:" + std::to_string(server->port()) + "/");

        DSBOX_NET_TLS_CHECK(!result.ok());
        DSBOX_NET_TLS_CHECK(result.error && result.error->code == NetErrorCode::Tls);
    }

    void testHttpsLargeBody()
    {
        const Pki pki = makePki();
        const std::size_t total = 3 * 1024 * 1024;

        auto context = makeServerContext(pki.server);

        LoopbackServer server([context, total](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);

            if (!tls.handshake())
                return;

            tls.readUntil("\r\n\r\n");
            tls.send("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(total) + "\r\n\r\n");

            const std::string block(32 * 1024, 'T');

            for (std::size_t sent = 0; sent < total; sent += block.size())
                tls.send(block);
        });

        Stack stack({ pemCertificate(pki.ca.cert.get()) });

        auto result = fetch(*stack.handler, "https://localhost:" + std::to_string(server.port()) + "/big");

        DSBOX_NET_TLS_CHECK(result.ok());

        if (!result.ok())
            return;

        const std::string body = bodyOf(result);

        DSBOX_NET_TLS_CHECK_EQ(body.size(), total);
        DSBOX_NET_TLS_CHECK(body.find_first_not_of('T') == std::string::npos);
    }

    void testTruncatedHttpsBodyIsDetected()
    {
        const Pki pki = makePki();
        auto context = makeServerContext(pki.server);

        LoopbackServer server([context](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);

            if (!tls.handshake())
                return;

            tls.readUntil("\r\n\r\n");
            tls.send("HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\nonly a bit");
            peer.close(); // dropped with no close_notify
        });

        Stack stack({ pemCertificate(pki.ca.cert.get()) });

        auto result = fetch(*stack.handler, "https://localhost:" + std::to_string(server.port()) + "/");

        DSBOX_NET_TLS_CHECK(result.ok());

        if (!result.ok())
            return;

        bool threw = false;

        try
        {
            bodyOf(result);
        }
        catch (const dsbox_net::NetException&)
        {
            threw = true;
        }

        DSBOX_NET_TLS_CHECK(threw);
    }

    void testHttpsUntilCloseBodyTruncationIsDetected()
    {
        // A response with no length, ended by a bare TCP close, is the
        // case where a missing close_notify is the ONLY sign that the
        // data was cut short.
        const Pki pki = makePki();
        auto context = makeServerContext(pki.server);

        LoopbackServer server([context](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);

            if (!tls.handshake())
                return;

            tls.readUntil("\r\n\r\n");
            tls.send("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nmaybe all of it");
            peer.close();
        });

        Stack stack({ pemCertificate(pki.ca.cert.get()) });

        auto result = fetch(*stack.handler, "https://localhost:" + std::to_string(server.port()) + "/");

        DSBOX_NET_TLS_CHECK(result.ok());

        if (!result.ok())
            return;

        bool threw = false;

        try
        {
            bodyOf(result);
        }
        catch (const dsbox_net::NetException&)
        {
            threw = true;
        }

        DSBOX_NET_TLS_CHECK(threw);
    }
}

void runHttpsTests()
{
    runTest("testHttpsGetThroughTheWholeStack", testHttpsGetThroughTheWholeStack);
    runTest("testHttpsConnectionIsReusedAcrossRequests", testHttpsConnectionIsReusedAcrossRequests);
    runTest("testHttpsToUntrustedServerReportsATlsError", testHttpsToUntrustedServerReportsATlsError);
    runTest("testHttpsLargeBody", testHttpsLargeBody);
    runTest("testTruncatedHttpsBodyIsDetected", testTruncatedHttpsBodyIsDetected);
    runTest("testHttpsUntilCloseBodyTruncationIsDetected", testHttpsUntilCloseBodyTruncationIsDetected);
}
