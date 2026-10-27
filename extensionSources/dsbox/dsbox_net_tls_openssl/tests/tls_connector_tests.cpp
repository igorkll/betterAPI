#include "test_framework.hpp"
#include "test_pki.hpp"

#include <dsbox_net_socket/tcp_connector.hpp>
#include <dsbox_net_tls/trust_manager.hpp>
#include <dsbox_net_tls_openssl/tls_connector.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dsbox_net_tls_openssl_tests;
using namespace dsbox_net_tls_openssl;
using namespace dsbox_test_pki;
using dsbox_net::NetErrorCode;
using dsbox_test_net::LoopbackServer;
using dsbox_test_net::RawConnection;

namespace
{
    using namespace std::chrono_literals;

    /*
     * Fixture helpers
     */

    // A CA plus a server certificate it issued, valid for localhost and
    // 127.0.0.1 unless told otherwise.
    struct Pki
    {
        Identity ca;
        Identity server;
    };

    Pki makePki(const CertSpec& serverSpec = []
    {
        CertSpec spec;
        spec.commonName = "localhost";
        spec.alternativeNames = "DNS:localhost,IP:127.0.0.1";
        return spec;
    }())
    {
        // Unique per CA: two CAs with the same subject and no key
        // identifiers cannot be told apart when an issuer is looked up,
        // which is not the situation under test.
        static int counter = 0;

        CertSpec caSpec;
        caSpec.commonName = "DSBox Test CA " + std::to_string(++counter);
        caSpec.isCa = true;

        Pki pki;
        pki.ca = issue(caSpec);
        pki.server = issue(serverSpec, &pki.ca);
        return pki;
    }

    std::shared_ptr<dsbox_net_socket::IConnector> tcp()
    {
        static const auto connector = std::make_shared<dsbox_net_socket::TcpConnector>();
        return connector;
    }

    struct ClientFixture
    {
        std::shared_ptr<dsbox_net_tls::TrustManager> trust = std::make_shared<dsbox_net_tls::TrustManager>();
        std::shared_ptr<FixedTrust> provider = std::make_shared<FixedTrust>();
        std::shared_ptr<TlsConnector> connector;

        explicit ClientFixture(std::vector<dsbox_net_tls::Certificate> trusted, TlsConnectorOptions options = {})
        {
            provider->set(std::move(trusted));
            trust->addProvider(provider);
            connector = std::make_shared<TlsConnector>(tcp(), trust, std::move(options));
        }
    };

    std::span<const std::byte> asBytes(const std::string& text)
    {
        return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
    }

    std::string readExactly(dsbox_net_socket::IConnection& connection, std::size_t count)
    {
        std::string result;
        std::vector<std::byte> buffer(8192);

        while (result.size() < count)
        {
            const std::size_t n = connection.read(buffer);

            if (n == 0)
                break;

            result.append(reinterpret_cast<const char*>(buffer.data()), n);
        }

        return result;
    }

    struct ConnectOutcome
    {
        std::shared_ptr<dsbox_net_socket::IConnection> connection;
        std::optional<dsbox_net::NetError> error;
    };

    ConnectOutcome tryConnect(TlsConnector& connector, const std::string& host, std::uint16_t port, dsbox_net_socket::ConnectOptions options = {})
    {
        ConnectOutcome outcome;

        try
        {
            outcome.connection = connector.connect(host, port, options);
        }
        catch (const dsbox_net::NetException& exception)
        {
            outcome.error = exception.error();
        }

        return outcome;
    }

    // A server that completes the handshake, then runs `session`.
    template<typename F>
    std::unique_ptr<LoopbackServer> tlsServer(const Pki& pki, F session)
    {
        auto context = makeServerContext(pki.server);

        return std::make_unique<LoopbackServer>([context, session](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);

            if (tls.handshake())
                session(tls);
        });
    }

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

    /*
     * Successful sessions
     */

    void testHandshakeAndExchangeByIpAddress()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls)
        {
            const std::string request = tls.readExact(4);
            tls.send("pong:" + request);
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (!outcome.connection)
            return;

        const std::string ping = "ping";
        outcome.connection->write(asBytes(ping));

        DSBOX_NET_TLS_CHECK_EQ(readExactly(*outcome.connection, 9), "pong:ping");
    }

    void testHandshakeByDnsName()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls)
        {
            tls.send("hello");
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "localhost", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (outcome.connection)
            DSBOX_NET_TLS_CHECK_EQ(readExactly(*outcome.connection, 5), "hello");
    }

    void testDerEncodedTrustAnchorIsAccepted()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.send("ok"); });

        ClientFixture client({ derCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());
    }

    void testServerCertificateItselfCanBeTheTrustAnchor()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.send("ok"); });

        ClientFixture client({ pemCertificate(pki.server.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        // A leaf trusted directly is accepted only if partial chains
        // are allowed; we deliberately do not allow them, so this must
        // fail. (Pinning a leaf is a different feature.)
        DSBOX_NET_TLS_CHECK(outcome.error.has_value());
    }

    void testSeveralProvidersCertificatesAreCombined()
    {
        const Pki pki = makePki();
        const Pki unrelated = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.send("ok"); });

        ClientFixture client({ pemCertificate(unrelated.ca.cert.get()), pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());
    }

    void testUnreadableTrustEntriesAreSkipped()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.send("ok"); });

        dsbox_net_tls::Certificate garbage;
        garbage.encoding = dsbox_net_tls::CertificateEncoding::Pem;
        garbage.data = { std::byte{ 'n' }, std::byte{ 'o' }, std::byte{ 'p' }, std::byte{ 'e' } };

        ClientFixture client({ garbage, pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());
    }

    void testLargeTransferBothWays()
    {
        const Pki pki = makePki();
        const std::size_t upload = 1024 * 1024;
        const std::size_t download = 3 * 1024 * 1024;

        std::atomic<std::size_t> serverReceived{ 0 };

        auto server = tlsServer(pki, [&](ServerSession& tls)
        {
            serverReceived = tls.readExact(upload).size();

            const std::string block(64 * 1024, 'd');

            for (std::size_t sent = 0; sent < download; sent += block.size())
                tls.send(block);
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (!outcome.connection)
            return;

        const std::string payload(upload, 'u');
        outcome.connection->write(asBytes(payload));

        const std::string received = readExactly(*outcome.connection, download);

        DSBOX_NET_TLS_CHECK_EQ(received.size(), download);
        DSBOX_NET_TLS_CHECK(received.find_first_not_of('d') == std::string::npos);
        DSBOX_NET_TLS_CHECK_EQ(serverReceived.load(), upload);
    }

    /*
     * Certificate and handshake failures
     */

    bool messageMentions(const ConnectOutcome& outcome, const std::string& text)
    {
        return outcome.error && outcome.error->message.find(text) != std::string::npos;
    }

    void testUntrustedServerIsRejected()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({ pemCertificate(makePki().ca.cert.get()) }); // trusts some other CA
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error.has_value());
        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
        DSBOX_NET_TLS_CHECK(messageMentions(outcome, "certificate verification failed"));
        DSBOX_NET_TLS_CHECK(outcome.connection == nullptr);
    }

    void testNoTrustProvidersMeansNothingIsTrusted()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({}); // an empty trust set must fail closed
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
        DSBOX_NET_TLS_CHECK(messageMentions(outcome, "no trusted certificates"));
    }

    void testHostnameMismatchIsRejected()
    {
        CertSpec spec;
        spec.commonName = "other.example";
        spec.alternativeNames = "DNS:other.example";

        const Pki pki = makePki(spec);
        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
        DSBOX_NET_TLS_CHECK(messageMentions(outcome, "certificate verification failed"));
    }

    void testIpAddressNotInCertificateIsRejected()
    {
        CertSpec spec;
        spec.commonName = "localhost";
        spec.alternativeNames = "DNS:localhost"; // no IP entry

        const Pki pki = makePki(spec);
        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
    }

    void testCommonNameAloneIsNotEnough()
    {
        // No subjectAltName at all: modern clients ignore the CN.
        CertSpec spec;
        spec.commonName = "localhost";

        const Pki pki = makePki(spec);
        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "localhost", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
    }

    void testExpiredCertificateIsRejected()
    {
        CertSpec spec;
        spec.commonName = "localhost";
        spec.alternativeNames = "DNS:localhost,IP:127.0.0.1";
        spec.notBeforeOffsetSeconds = -7200;
        spec.notAfterOffsetSeconds = -3600;

        const Pki pki = makePki(spec);
        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
        DSBOX_NET_TLS_CHECK(messageMentions(outcome, "expired"));
    }

    void testNotYetValidCertificateIsRejected()
    {
        CertSpec spec;
        spec.commonName = "localhost";
        spec.alternativeNames = "DNS:localhost,IP:127.0.0.1";
        spec.notBeforeOffsetSeconds = 3600;
        spec.notAfterOffsetSeconds = 7200;

        const Pki pki = makePki(spec);
        auto server = tlsServer(pki, [](ServerSession&) {});

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
    }

    void testPlainTextServerFailsTheHandshake()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readExact(1);
            peer.send("HTTP/1.1 400 Bad Request\r\n\r\n");
        });

        const Pki pki = makePki();
        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Tls);
    }

    void testServerThatClosesImmediatelyFailsTheHandshake()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.close();
        });

        const Pki pki = makePki();
        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port());

        DSBOX_NET_TLS_CHECK(outcome.error.has_value());
        DSBOX_NET_TLS_CHECK(outcome.connection == nullptr);
    }

    void testProtocolOlderThanTls12IsRefused()
    {
        const Pki pki = makePki();

        std::shared_ptr<SSL_CTX> context = makeServerContext(pki.server);
        SSL_CTX_set_cipher_list(context.get(), "DEFAULT:@SECLEVEL=0");
        SSL_CTX_set_min_proto_version(context.get(), TLS1_VERSION);
        SSL_CTX_set_max_proto_version(context.get(), TLS1_1_VERSION);

        LoopbackServer server([context](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);
            tls.handshake();
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port());

        DSBOX_NET_TLS_CHECK(outcome.error.has_value());
    }

    /*
     * Timeouts and cancellation
     */

    void testSilentServerTimesOutTheHandshake()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readToEnd(); // accepts, then says nothing
        });

        const Pki pki = makePki();
        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });

        dsbox_net_socket::ConnectOptions options;
        options.timeout = 300ms;

        const auto started = std::chrono::steady_clock::now();
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port(), options);
        const auto elapsed = std::chrono::steady_clock::now() - started;

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Timeout);
        DSBOX_NET_TLS_CHECK(elapsed < 5s);
    }

    void testHandshakeTimeoutOptionApplies()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readToEnd();
        });

        const Pki pki = makePki();

        TlsConnectorOptions tlsOptions;
        tlsOptions.handshakeTimeout = 250ms;

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) }, tlsOptions);

        const auto started = std::chrono::steady_clock::now();
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port());

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Timeout);
        DSBOX_NET_TLS_CHECK(std::chrono::steady_clock::now() - started < 5s);
    }

    void testCancelDuringHandshake()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readToEnd();
        });

        const Pki pki = makePki();
        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });

        dsbox_net::CancellationSource source;
        dsbox_net_socket::ConnectOptions options;
        options.cancel = source.token();

        std::thread canceller([&]
        {
            std::this_thread::sleep_for(200ms);
            source.cancel();
        });

        const auto started = std::chrono::steady_clock::now();
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port(), options);
        const auto elapsed = std::chrono::steady_clock::now() - started;

        canceller.join();

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Cancelled);
        DSBOX_NET_TLS_CHECK(elapsed < 5s);
    }

    void testCancelReasonIsPropagatedFromHandshake()
    {
        LoopbackServer server([](RawConnection& peer)
        {
            peer.readToEnd();
        });

        const Pki pki = makePki();
        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });

        dsbox_net::CancellationSource source;
        dsbox_net_socket::ConnectOptions options;
        options.cancel = source.token();

        std::thread canceller([&]
        {
            std::this_thread::sleep_for(150ms);
            source.cancel(NetErrorCode::Timeout);
        });

        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port(), options);
        canceller.join();

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Timeout);
    }

    void testConnectFailureOfTransportIsPassedThrough()
    {
        std::uint16_t deadPort = 0;

        {
            LoopbackServer server([](RawConnection&) {});
            deadPort = server.port();
        }

        const Pki pki = makePki();
        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", deadPort);

        DSBOX_NET_TLS_CHECK(outcome.error && outcome.error->code == NetErrorCode::Connect);
    }

    /*
     * Behaviour of an established connection
     */

    void testCloseUnblocksAWaitingRead()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls)
        {
            tls.readExact(1); // waits until the client goes away
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (!outcome.connection)
            return;

        auto connection = outcome.connection;
        std::atomic<bool> returned{ false };
        std::size_t got = 99;

        std::thread reader([&]
        {
            std::byte buffer[8];
            got = connection->read(buffer);
            returned = true;
        });

        std::this_thread::sleep_for(150ms);
        DSBOX_NET_TLS_CHECK(!returned);

        connection->close();
        reader.join();

        DSBOX_NET_TLS_CHECK(returned);
        DSBOX_NET_TLS_CHECK_EQ(got, static_cast<std::size_t>(0));
    }

    void testWriteAfterCloseThrows()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.readExact(1); });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (!outcome.connection)
            return;

        outcome.connection->close();

        const std::string data = "x";
        const auto code = errorCodeOf([&] { outcome.connection->write(asBytes(data)); });

        DSBOX_NET_TLS_CHECK(code == NetErrorCode::ConnectionReset);
    }

    void testCloseNotifyIsACleanEndOfStream()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls)
        {
            tls.send("bye");
            tls.shutdown(); // close_notify
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (!outcome.connection)
            return;

        DSBOX_NET_TLS_CHECK_EQ(readExactly(*outcome.connection, 100), "bye");

        std::byte buffer[8];
        DSBOX_NET_TLS_CHECK_EQ(outcome.connection->read(buffer), static_cast<std::size_t>(0));
    }

    void testDroppedConnectionWithoutCloseNotifyIsAnError()
    {
        const Pki pki = makePki();

        auto context = makeServerContext(pki.server);

        LoopbackServer server([context](RawConnection& peer)
        {
            ServerSession tls(context.get(), peer);

            if (!tls.handshake())
                return;

            tls.send("partial");
            peer.close(); // the TCP connection just ends: no close_notify
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });
        auto outcome = tryConnect(*client.connector, "127.0.0.1", server.port());

        DSBOX_NET_TLS_CHECK(!outcome.error.has_value());

        if (!outcome.connection)
            return;

        // What arrived before the cut is delivered; then the cut itself
        // is reported -- not mistaken for a normal end of the data.
        std::string received;
        std::optional<NetErrorCode> code;

        try
        {
            std::vector<std::byte> buffer(64);

            for (;;)
            {
                const std::size_t n = outcome.connection->read(buffer);

                if (n == 0)
                    break;

                received.append(reinterpret_cast<const char*>(buffer.data()), n);
            }
        }
        catch (const dsbox_net::NetException& exception)
        {
            code = exception.error().code;
        }

        DSBOX_NET_TLS_CHECK_EQ(received, "partial");
        DSBOX_NET_TLS_CHECK(code == NetErrorCode::ConnectionReset);
    }

    void testDestroyingTheConnectionSendsCloseNotify()
    {
        const Pki pki = makePki();

        // 0 = still running, 1 = the stream ended with close_notify,
        // 2 = it ended without.
        std::atomic<int> endOfSession{ 0 };

        auto server = tlsServer(pki, [&](ServerSession& tls)
        {
            tls.readExact(1); // returns when the client's side of the stream ends
            endOfSession = tls.receivedCloseNotify() ? 1 : 2;
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });

        {
            auto outcome = tryConnect(*client.connector, "127.0.0.1", server->port());
            DSBOX_NET_TLS_CHECK(!outcome.error.has_value());
        } // the connection is destroyed here

        for (int i = 0; i < 100 && endOfSession == 0; ++i)
            std::this_thread::sleep_for(20ms);

        DSBOX_NET_TLS_CHECK_EQ(endOfSession.load(), 1);
    }

    /*
     * Trust set refreshing
     */

    void testTrustIsReReadWhenIntervalIsZero()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.send("ok"); });

        TlsConnectorOptions options;
        options.trustRefreshInterval = 0ms;

        ClientFixture client({}, options);

        auto first = tryConnect(*client.connector, "127.0.0.1", server->port());
        DSBOX_NET_TLS_CHECK(first.error.has_value());

        client.provider->set({ pemCertificate(pki.ca.cert.get()) });

        auto second = tryConnect(*client.connector, "127.0.0.1", server->port());
        DSBOX_NET_TLS_CHECK(!second.error.has_value());
    }

    void testTrustIsCachedUntilRefreshed()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls) { tls.send("ok"); });

        TlsConnectorOptions options;
        options.trustRefreshInterval = std::chrono::hours(1);

        ClientFixture client({}, options);

        auto first = tryConnect(*client.connector, "127.0.0.1", server->port());
        DSBOX_NET_TLS_CHECK(first.error.has_value());

        client.provider->set({ pemCertificate(pki.ca.cert.get()) });

        // Still the old, empty set: it was cached.
        auto cached = tryConnect(*client.connector, "127.0.0.1", server->port());
        DSBOX_NET_TLS_CHECK(cached.error.has_value());

        client.connector->refreshTrust();

        auto refreshed = tryConnect(*client.connector, "127.0.0.1", server->port());
        DSBOX_NET_TLS_CHECK(!refreshed.error.has_value());
    }

    void testConstructorRejectsNullArguments()
    {
        auto trust = std::make_shared<dsbox_net_tls::TrustManager>();

        bool threwForTransport = false;
        bool threwForTrust = false;

        try { TlsConnector connector(nullptr, trust); }
        catch (const std::invalid_argument&) { threwForTransport = true; }

        try { TlsConnector connector(tcp(), nullptr); }
        catch (const std::invalid_argument&) { threwForTrust = true; }

        DSBOX_NET_TLS_CHECK(threwForTransport);
        DSBOX_NET_TLS_CHECK(threwForTrust);
    }

    void testConcurrentConnections()
    {
        const Pki pki = makePki();

        auto server = tlsServer(pki, [](ServerSession& tls)
        {
            const std::string request = tls.readUntil("\n");
            tls.send("echo:" + request);
        });

        ClientFixture client({ pemCertificate(pki.ca.cert.get()) });

        std::atomic<int> failures{ 0 };
        std::vector<std::thread> threads;

        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back([&, t]
            {
                for (int i = 0; i < 5; ++i)
                {
                    const std::string line = "msg" + std::to_string(t) + "-" + std::to_string(i) + "\n";

                    try
                    {
                        auto connection = client.connector->connect("127.0.0.1", server->port(), {});
                        connection->write(asBytes(line));

                        if (readExactly(*connection, line.size() + 5) != "echo:" + line)
                            ++failures;
                    }
                    catch (...)
                    {
                        ++failures;
                    }
                }
            });
        }

        for (auto& thread : threads)
            thread.join();

        DSBOX_NET_TLS_CHECK_EQ(failures.load(), 0);
    }
}

void runTlsConnectorTests()
{
    runTest("testHandshakeAndExchangeByIpAddress", testHandshakeAndExchangeByIpAddress);
    runTest("testHandshakeByDnsName", testHandshakeByDnsName);
    runTest("testDerEncodedTrustAnchorIsAccepted", testDerEncodedTrustAnchorIsAccepted);
    runTest("testServerCertificateItselfCanBeTheTrustAnchor", testServerCertificateItselfCanBeTheTrustAnchor);
    runTest("testSeveralProvidersCertificatesAreCombined", testSeveralProvidersCertificatesAreCombined);
    runTest("testUnreadableTrustEntriesAreSkipped", testUnreadableTrustEntriesAreSkipped);
    runTest("testLargeTransferBothWays", testLargeTransferBothWays);

    runTest("testUntrustedServerIsRejected", testUntrustedServerIsRejected);
    runTest("testNoTrustProvidersMeansNothingIsTrusted", testNoTrustProvidersMeansNothingIsTrusted);
    runTest("testHostnameMismatchIsRejected", testHostnameMismatchIsRejected);
    runTest("testIpAddressNotInCertificateIsRejected", testIpAddressNotInCertificateIsRejected);
    runTest("testCommonNameAloneIsNotEnough", testCommonNameAloneIsNotEnough);
    runTest("testExpiredCertificateIsRejected", testExpiredCertificateIsRejected);
    runTest("testNotYetValidCertificateIsRejected", testNotYetValidCertificateIsRejected);
    runTest("testPlainTextServerFailsTheHandshake", testPlainTextServerFailsTheHandshake);
    runTest("testServerThatClosesImmediatelyFailsTheHandshake", testServerThatClosesImmediatelyFailsTheHandshake);
    runTest("testProtocolOlderThanTls12IsRefused", testProtocolOlderThanTls12IsRefused);

    runTest("testSilentServerTimesOutTheHandshake", testSilentServerTimesOutTheHandshake);
    runTest("testHandshakeTimeoutOptionApplies", testHandshakeTimeoutOptionApplies);
    runTest("testCancelDuringHandshake", testCancelDuringHandshake);
    runTest("testCancelReasonIsPropagatedFromHandshake", testCancelReasonIsPropagatedFromHandshake);
    runTest("testConnectFailureOfTransportIsPassedThrough", testConnectFailureOfTransportIsPassedThrough);

    runTest("testCloseUnblocksAWaitingRead", testCloseUnblocksAWaitingRead);
    runTest("testWriteAfterCloseThrows", testWriteAfterCloseThrows);
    runTest("testCloseNotifyIsACleanEndOfStream", testCloseNotifyIsACleanEndOfStream);
    runTest("testDroppedConnectionWithoutCloseNotifyIsAnError", testDroppedConnectionWithoutCloseNotifyIsAnError);
    runTest("testDestroyingTheConnectionSendsCloseNotify", testDestroyingTheConnectionSendsCloseNotify);

    runTest("testTrustIsReReadWhenIntervalIsZero", testTrustIsReReadWhenIntervalIsZero);
    runTest("testTrustIsCachedUntilRefreshed", testTrustIsCachedUntilRefreshed);
    runTest("testConstructorRejectsNullArguments", testConstructorRejectsNullArguments);
    runTest("testConcurrentConnections", testConcurrentConnections);
}
