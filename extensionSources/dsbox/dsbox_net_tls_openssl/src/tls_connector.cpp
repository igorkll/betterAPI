#include <dsbox_net_tls_openssl/tls_connector.hpp>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace dsbox_net_tls_openssl
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        using dsbox_net::NetError;
        using dsbox_net::NetErrorCode;
        using dsbox_net::NetException;
        using dsbox_net_socket::ConnectOptions;
        using dsbox_net_socket::IConnection;
        using dsbox_net_socket::IConnector;

        constexpr std::size_t kMaxIoChunk = 1u << 20;

        /*
         * Small OpenSSL RAII helpers
         */

        struct SslCtxDeleter
        {
            void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
        };

        struct SslDeleter
        {
            void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
        };

        struct X509Deleter
        {
            void operator()(X509* certificate) const noexcept { X509_free(certificate); }
        };

        struct BioDeleter
        {
            void operator()(BIO* bio) const noexcept { BIO_free(bio); }
        };

        void ensureOpenSslInitialised()
        {
            static const int initialised = OPENSSL_init_ssl(
                OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                nullptr);

            (void)initialised;
        }

        // Empties this thread's OpenSSL error queue into one string.
        std::string drainErrorQueue()
        {
            std::string text;

            while (const unsigned long code = ERR_get_error())
            {
                char buffer[256];
                ERR_error_string_n(code, buffer, sizeof(buffer));

                if (!text.empty())
                    text += "; ";

                text += buffer;
            }

            return text;
        }

        NetException makeException(NetErrorCode code, std::string message, int nativeCode = 0)
        {
            return NetException(NetError{ code, std::move(message), nativeCode });
        }

        std::string stripBrackets(const std::string& host)
        {
            if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
                return host.substr(1, host.size() - 2);

            return host;
        }

        /*
         * A BIO that reads and writes through an IConnection, so OpenSSL
         * runs over whatever transport the connector was given -- not
         * necessarily an operating-system socket.
         */

        struct TransportContext
        {
            IConnection* transport = nullptr;
            std::optional<NetError> ioError; // the transport threw
            bool endOfStream = false;        // the transport reported EOF
        };

        int bioWrite(BIO* bio, const char* data, int length)
        {
            BIO_clear_retry_flags(bio);

            auto* context = static_cast<TransportContext*>(BIO_get_data(bio));

            if (!context || !context->transport || length <= 0)
                return 0;

            try
            {
                context->transport->write(std::span<const std::byte>(
                    reinterpret_cast<const std::byte*>(data), static_cast<std::size_t>(length)));
                return length;
            }
            catch (const NetException& exception)
            {
                context->ioError = exception.error();
            }
            catch (...)
            {
                context->ioError = NetError{ NetErrorCode::Internal, "unexpected error writing to the transport", 0 };
            }

            return -1;
        }

        int bioRead(BIO* bio, char* data, int length)
        {
            BIO_clear_retry_flags(bio);

            auto* context = static_cast<TransportContext*>(BIO_get_data(bio));

            if (!context || !context->transport || length <= 0)
                return 0;

            try
            {
                const std::size_t n = context->transport->read(std::span<std::byte>(
                    reinterpret_cast<std::byte*>(data), static_cast<std::size_t>(length)));

                if (n == 0)
                {
                    context->endOfStream = true;
                    return 0;
                }

                return static_cast<int>(n);
            }
            catch (const NetException& exception)
            {
                context->ioError = exception.error();
            }
            catch (...)
            {
                context->ioError = NetError{ NetErrorCode::Internal, "unexpected error reading from the transport", 0 };
            }

            return -1;
        }

        long bioCtrl(BIO* bio, int command, long, void*)
        {
            switch (command)
            {
            case BIO_CTRL_FLUSH:
                return 1;

            case BIO_CTRL_EOF:
            {
                const auto* context = static_cast<const TransportContext*>(BIO_get_data(bio));
                return context && context->endOfStream ? 1 : 0;
            }

            default:
                return 0;
            }
        }

        int bioCreate(BIO* bio)
        {
            BIO_set_init(bio, 1);
            return 1;
        }

        int bioDestroy(BIO* bio)
        {
            if (!bio)
                return 0;

            BIO_set_data(bio, nullptr);
            BIO_set_init(bio, 0);
            return 1;
        }

        BIO_METHOD* transportBioMethod()
        {
            // Created once and kept for the life of the process, as
            // OpenSSL recommends for custom BIO methods.
            static BIO_METHOD* method = []
            {
                BIO_METHOD* created = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "dsbox transport");

                if (created)
                {
                    BIO_meth_set_write(created, bioWrite);
                    BIO_meth_set_read(created, bioRead);
                    BIO_meth_set_ctrl(created, bioCtrl);
                    BIO_meth_set_create(created, bioCreate);
                    BIO_meth_set_destroy(created, bioDestroy);
                }

                return created;
            }();

            return method;
        }

        /*
         * Watchdog: one thread that runs a callback when a deadline
         * passes -- how a blocked handshake is aborted on timeout.
         */

        class Watchdog
        {
        public:
            Watchdog() = default;

            ~Watchdog()
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopping = true;
                }

                m_wake.notify_all();

                if (m_thread.joinable())
                    m_thread.join();
            }

            Watchdog(const Watchdog&) = delete;
            Watchdog& operator=(const Watchdog&) = delete;

            std::uint64_t arm(Clock::time_point deadline, std::function<void()> action)
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (!m_thread.joinable())
                    m_thread = std::thread([this] { loop(); });

                const std::uint64_t id = m_nextId++;
                m_entries.push_back(Entry{ id, deadline, std::move(action) });
                m_wake.notify_all();
                return id;
            }

            // After this returns, the action for `id` is not running and
            // never will: if it was already running, this waits for it.
            void disarm(std::uint64_t id)
            {
                std::unique_lock<std::mutex> lock(m_mutex);

                m_entries.erase(
                    std::remove_if(m_entries.begin(), m_entries.end(), [id](const Entry& entry)
                    {
                        return entry.id == id;
                    }),
                    m_entries.end());

                m_idle.wait(lock, [&] { return m_runningId != id; });
            }

        private:
            struct Entry
            {
                std::uint64_t id;
                Clock::time_point deadline;
                std::function<void()> action;
            };

            void loop()
            {
                std::unique_lock<std::mutex> lock(m_mutex);

                while (!m_stopping)
                {
                    if (m_entries.empty())
                    {
                        m_wake.wait(lock);
                        continue;
                    }

                    const auto earliest = std::min_element(
                        m_entries.begin(),
                        m_entries.end(),
                        [](const Entry& a, const Entry& b) { return a.deadline < b.deadline; });

                    if (Clock::now() < earliest->deadline)
                    {
                        m_wake.wait_until(lock, earliest->deadline);
                        continue;
                    }

                    Entry due = std::move(*earliest);
                    m_entries.erase(earliest);

                    m_runningId = due.id;
                    lock.unlock();

                    try
                    {
                        due.action();
                    }
                    catch (...)
                    {
                    }

                    lock.lock();
                    m_runningId = 0;
                    m_idle.notify_all();
                }
            }

            std::mutex m_mutex;
            std::condition_variable m_wake;
            std::condition_variable m_idle;
            std::vector<Entry> m_entries;
            std::uint64_t m_nextId = 1;
            std::uint64_t m_runningId = 0;
            bool m_stopping = false;
            std::thread m_thread;
        };

        /*
         * One TLS session over a transport connection
         */

        class TlsConnection final : public IConnection
        {
        public:
            explicit TlsConnection(std::shared_ptr<IConnection> transport)
                : m_transport(std::move(transport))
            {
                m_context.transport = m_transport.get();
            }

            ~TlsConnection() override
            {
                // Tell the peer we are done, once, without waiting for
                // its answer -- but not on a connection that was
                // aborted or has already failed.
                if (m_ssl && m_established && !m_closed.load() && !m_context.ioError)
                {
                    ERR_clear_error();
                    SSL_shutdown(m_ssl.get());
                    ERR_clear_error();
                }
            }

            // Runs the client handshake. Throws NetException(Tls) -- or
            // the transport's own error -- on failure.
            void handshake(SSL_CTX* context, const std::string& host, std::size_t trustedCertificates)
            {
                m_ssl.reset(SSL_new(context));

                if (!m_ssl)
                    throw makeException(NetErrorCode::Tls, "could not create a TLS session: " + drainErrorQueue());

                BIO* bio = BIO_new(transportBioMethod());

                if (!bio)
                    throw makeException(NetErrorCode::Tls, "could not create the TLS transport: " + drainErrorQueue());

                BIO_set_data(bio, &m_context);
                SSL_set_bio(m_ssl.get(), bio, bio); // the session now owns the BIO

                // The name to verify the certificate against: an IP
                // literal is matched against the certificate's IP
                // addresses, anything else against its DNS names. Only
                // a DNS name is also sent as SNI (RFC 6066 forbids IP
                // literals there).
                X509_VERIFY_PARAM* parameters = SSL_get0_param(m_ssl.get());
                // No fallback to the certificate's Common Name, as in
                // browsers: a name must appear in subjectAltName.
                X509_VERIFY_PARAM_set_hostflags(
                    parameters,
                    X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);

                if (X509_VERIFY_PARAM_set1_ip_asc(parameters, host.c_str()) != 1)
                {
                    ERR_clear_error();

                    if (SSL_set1_host(m_ssl.get(), host.c_str()) != 1)
                        throw makeException(NetErrorCode::Tls, "invalid host name for certificate verification: " + host);

                    SSL_set_tlsext_host_name(m_ssl.get(), host.c_str());
                }

                SSL_set_connect_state(m_ssl.get());

                ERR_clear_error();
                const int result = SSL_connect(m_ssl.get());

                if (result == 1)
                {
                    m_established = true;
                    return;
                }

                const int sslError = SSL_get_error(m_ssl.get(), result);
                const std::string queued = drainErrorQueue();
                const long verifyResult = SSL_get_verify_result(m_ssl.get());

                std::string message = "TLS handshake with " + host + " failed";

                if (verifyResult != X509_V_OK)
                {
                    message += ": certificate verification failed: ";
                    message += X509_verify_cert_error_string(verifyResult);

                    if (trustedCertificates == 0)
                        message += " (no trusted certificates are registered -- is a trust provider module loaded?)";
                }
                else if (m_context.ioError)
                {
                    message += ": " + m_context.ioError->message;
                }
                else if (m_context.endOfStream)
                {
                    message += ": the server closed the connection";
                }
                else if (!queued.empty())
                {
                    message += ": " + queued;
                }

                throw makeException(NetErrorCode::Tls, std::move(message), sslError);
            }

            std::size_t read(std::span<std::byte> out) override
            {
                if (out.empty() || m_closed.load())
                    return 0;

                const int want = static_cast<int>(std::min<std::size_t>(out.size(), kMaxIoChunk));

                for (;;)
                {
                    ERR_clear_error();
                    const int n = SSL_read(m_ssl.get(), out.data(), want);

                    if (n > 0)
                        return static_cast<std::size_t>(n);

                    const int sslError = SSL_get_error(m_ssl.get(), n);

                    if (sslError == SSL_ERROR_ZERO_RETURN)
                        return 0; // the peer said close_notify: a clean end

                    if (m_closed.load())
                        return 0; // our own close() woke us

                    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
                        continue; // not expected over a blocking transport

                    throw failure("read", sslError);
                }
            }

            void write(std::span<const std::byte> data) override
            {
                while (!data.empty())
                {
                    if (m_closed.load())
                        throw makeException(NetErrorCode::ConnectionReset, "connection closed");

                    const int chunk = static_cast<int>(std::min<std::size_t>(data.size(), kMaxIoChunk));

                    ERR_clear_error();
                    const int n = SSL_write(m_ssl.get(), data.data(), chunk);

                    if (n > 0)
                    {
                        data = data.subspan(static_cast<std::size_t>(n));
                        continue;
                    }

                    const int sslError = SSL_get_error(m_ssl.get(), n);

                    if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE)
                        continue;

                    throw failure("write", sslError);
                }
            }

            // Only the transport is touched: the SSL object is not safe
            // to use from a second thread, and a blocked SSL_read is
            // woken by its transport read returning anyway.
            void close() noexcept override
            {
                if (!m_closed.exchange(true))
                    m_transport->close();
            }

        private:
            NetException failure(const char* operation, int sslError)
            {
                const std::string queued = drainErrorQueue();

                if (m_context.ioError)
                {
                    NetError error = *m_context.ioError;
                    error.message = std::string("TLS ") + operation + " failed: " + error.message;
                    return NetException(std::move(error));
                }

                if (m_context.endOfStream)
                {
                    return makeException(
                        NetErrorCode::ConnectionReset,
                        "the connection was closed without a TLS close_notify (the data may be truncated)",
                        sslError);
                }

                return makeException(
                    NetErrorCode::Tls,
                    std::string("TLS ") + operation + " failed" + (queued.empty() ? std::string() : ": " + queued),
                    sslError);
            }

            // Declaration order matters: the SSL session (and with it
            // its BIO, which points at m_context) is destroyed first,
            // then the context, then the transport.
            std::shared_ptr<IConnection> m_transport;
            TransportContext m_context;
            std::unique_ptr<SSL, SslDeleter> m_ssl;
            std::atomic<bool> m_closed{ false };
            bool m_established = false;
        };

        /*
         * Building the SSL_CTX from the trust manager's certificates
         */

        std::unique_ptr<X509, X509Deleter> parseCertificate(const dsbox_net_tls::Certificate& certificate)
        {
            if (certificate.data.empty())
                return nullptr;

            if (certificate.encoding == dsbox_net_tls::CertificateEncoding::Pem)
            {
                std::unique_ptr<BIO, BioDeleter> bio(BIO_new_mem_buf(
                    certificate.data.data(), static_cast<int>(certificate.data.size())));

                if (!bio)
                    return nullptr;

                return std::unique_ptr<X509, X509Deleter>(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
            }

            const unsigned char* cursor = reinterpret_cast<const unsigned char*>(certificate.data.data());
            return std::unique_ptr<X509, X509Deleter>(
                d2i_X509(nullptr, &cursor, static_cast<long>(certificate.data.size())));
        }

        std::vector<unsigned char> encodeAlpn(const std::vector<std::string>& protocols)
        {
            std::vector<unsigned char> wire;

            for (const std::string& protocol : protocols)
            {
                if (protocol.empty() || protocol.size() > 255)
                    continue;

                wire.push_back(static_cast<unsigned char>(protocol.size()));
                wire.insert(wire.end(), protocol.begin(), protocol.end());
            }

            return wire;
        }
    }

    /*
     * TlsConnector::Impl
     */

    struct TlsConnector::Impl
    {
        Impl(
            std::shared_ptr<IConnector> transportIn,
            std::shared_ptr<dsbox_net_tls::TrustManager> trustIn,
            TlsConnectorOptions optionsIn)
            : transport(std::move(transportIn))
            , trust(std::move(trustIn))
            , options(std::move(optionsIn))
        {
        }

        struct ContextHandle
        {
            std::shared_ptr<SSL_CTX> context;
            std::size_t trustedCertificates = 0;
        };

        ContextHandle currentContext()
        {
            std::lock_guard<std::mutex> lock(mutex);

            const auto now = Clock::now();
            const bool fresh = cached.context
                && !refreshRequested
                && options.trustRefreshInterval.count() > 0
                && now - builtAt < options.trustRefreshInterval;

            if (!fresh)
            {
                cached = build();
                builtAt = now;
                refreshRequested = false;
            }

            return cached;
        }

        ContextHandle build() const
        {
            ensureOpenSslInitialised();
            ERR_clear_error();

            std::shared_ptr<SSL_CTX> context(SSL_CTX_new(TLS_client_method()), SslCtxDeleter{});

            if (!context)
                throw makeException(NetErrorCode::Tls, "could not create a TLS context: " + drainErrorQueue());

            if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1)
                throw makeException(NetErrorCode::Tls, "could not restrict TLS to 1.2 and newer: " + drainErrorQueue());

            SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION);
            SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);

            const auto alpn = encodeAlpn(options.alpnProtocols);

            if (!alpn.empty())
                SSL_CTX_set_alpn_protos(context.get(), alpn.data(), static_cast<unsigned int>(alpn.size()));

            X509_STORE* store = X509_STORE_new();

            if (!store)
                throw makeException(NetErrorCode::Tls, "could not create a certificate store: " + drainErrorQueue());

            std::size_t added = 0;

            for (const auto& certificate : trust->collectAll())
            {
                const auto parsed = parseCertificate(certificate);

                if (!parsed)
                {
                    ERR_clear_error(); // unreadable entry: skip it, trust nothing from it
                    continue;
                }

                if (X509_STORE_add_cert(store, parsed.get()) == 1)
                    ++added;

                ERR_clear_error(); // a duplicate is reported as an error; harmless
            }

            SSL_CTX_set_cert_store(context.get(), store); // the context owns it now

            ContextHandle handle;
            handle.context = std::move(context);
            handle.trustedCertificates = added;
            return handle;
        }

        std::shared_ptr<IConnector> transport;
        std::shared_ptr<dsbox_net_tls::TrustManager> trust;
        TlsConnectorOptions options;

        std::mutex mutex;
        ContextHandle cached;
        Clock::time_point builtAt;
        bool refreshRequested = false;

        // Last: destroyed first, so its thread is gone before anything
        // its callbacks might touch.
        Watchdog watchdog;
    };

    TlsConnector::TlsConnector(
        std::shared_ptr<IConnector> transport,
        std::shared_ptr<dsbox_net_tls::TrustManager> trust,
        TlsConnectorOptions options)
    {
        if (!transport)
            throw std::invalid_argument("TlsConnector: transport connector must not be null");

        if (!trust)
            throw std::invalid_argument("TlsConnector: trust manager must not be null");

        m_impl = std::make_unique<Impl>(std::move(transport), std::move(trust), std::move(options));
    }

    TlsConnector::~TlsConnector() = default;

    void TlsConnector::refreshTrust()
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->refreshRequested = true;
    }

    std::shared_ptr<IConnection> TlsConnector::connect(
        const std::string& host,
        std::uint16_t port,
        const ConnectOptions& options)
    {
        Impl& impl = *m_impl;
        const std::string hostName = stripBrackets(host);

        const std::shared_ptr<IConnection> transport = impl.transport->connect(host, port, options);

        const auto context = impl.currentContext();
        auto connection = std::make_shared<TlsConnection>(transport);

        // From here, anything that closes the transport -- a timeout, a
        // cancel -- makes the blocked handshake fail, and we then work
        // out which of them it was.
        std::atomic<bool> expired{ false };

        struct Disarm
        {
            Watchdog& watchdog;
            std::uint64_t id = 0;

            ~Disarm()
            {
                if (id != 0)
                    watchdog.disarm(id);
            }
        } disarm{ impl.watchdog };

        const auto timeout = options.timeout.count() > 0 ? options.timeout : impl.options.handshakeTimeout;

        if (timeout.count() > 0)
        {
            disarm.id = impl.watchdog.arm(Clock::now() + timeout, [&expired, transport]
            {
                expired = true;
                transport->close();
            });
        }

        const auto cancelSubscription = options.cancel.onCancel([transport]
        {
            transport->close();
        });

        try
        {
            connection->handshake(context.context.get(), hostName, context.trustedCertificates);

            // Stop the clock BEFORE judging the outcome, so a timeout
            // that fires at the very last moment cannot slip a
            // just-closed connection past us.
            if (disarm.id != 0)
            {
                impl.watchdog.disarm(disarm.id);
                disarm.id = 0;
            }

            if (expired.load())
                throw makeException(NetErrorCode::Timeout, "TLS handshake with " + hostName + " timed out");
        }
        catch (const NetException&)
        {
            if (options.cancel.isCancelled())
                throw makeException(options.cancel.reason(), "TLS handshake with " + hostName + " cancelled");

            if (expired.load())
                throw makeException(NetErrorCode::Timeout, "TLS handshake with " + hostName + " timed out");

            throw;
        }

        return connection;
    }
}
