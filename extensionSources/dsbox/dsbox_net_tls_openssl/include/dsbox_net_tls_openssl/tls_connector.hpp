#pragma once

#include <dsbox_net_tls_openssl/api.hpp>

#include <dsbox_net_socket/connection.hpp>
#include <dsbox_net_tls/trust_manager.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace dsbox_net_tls_openssl
{
    struct TlsConnectorOptions
    {
        // Upper bound on the TLS handshake, used when
        // ConnectOptions::timeout is zero. (The TCP connect that
        // precedes it has its own timeout, set by the transport.)
        std::chrono::milliseconds handshakeTimeout{ 15000 };

        // How long the set of trusted certificates, gathered from the
        // TrustManager, is reused before being gathered again. Zero
        // gathers it for every connection. Certificates added by a
        // trust provider show up in new connections within this time
        // (or at once after refreshTrust()).
        std::chrono::milliseconds trustRefreshInterval{ 30000 };

        // Application protocols offered via ALPN, in preference order.
        std::vector<std::string> alpnProtocols{ "http/1.1" };
    };

    // Opens TLS connections with OpenSSL, over connections obtained from
    // another IConnector (normally the plain-TCP one): connect() makes
    // the underlying connection, performs the client handshake, and
    // returns an IConnection that encrypts and decrypts transparently.
    //
    // What it enforces, with no way to switch any of it off:
    //   - TLS 1.2 or newer;
    //   - the server's certificate chain must lead to a certificate in
    //     the TrustManager's collected set (collectAll()) -- if no
    //     provider is registered, nothing is trusted and every
    //     handshake fails, rather than silently trusting everything;
    //   - the certificate must be valid for the host that was asked for
    //     (DNS name or IP address), and not expired.
    //
    // The IConnection it returns follows the IConnection contract, with
    // two TLS-specific points: close() aborts the transport without
    // sending a TLS close_notify (it must be callable from any thread,
    // and the TLS state is not), while destroying a connection that was
    // not close()d sends one; and a peer that drops the connection
    // without close_notify is reported as ConnectionReset rather than as
    // a clean end of stream, so a truncated body cannot masquerade as a
    // complete one.
    //
    // Thread-safe.
    class DSBOX_NET_TLS_OPENSSL_API TlsConnector final : public dsbox_net_socket::IConnector
    {
    public:
        TlsConnector(
            std::shared_ptr<dsbox_net_socket::IConnector> transport,
            std::shared_ptr<dsbox_net_tls::TrustManager> trust,
            TlsConnectorOptions options = {});

        ~TlsConnector() override;

        TlsConnector(const TlsConnector&) = delete;
        TlsConnector& operator=(const TlsConnector&) = delete;

        // Throws dsbox_net::NetException: whatever the transport throws
        // (Dns, Connect, Timeout, Cancelled, ...), Tls for a failed
        // handshake or certificate verification (the message says
        // which), Timeout if the handshake took too long, Cancelled (or
        // the token's reason) if the options' token fired.
        std::shared_ptr<dsbox_net_socket::IConnection> connect(
            const std::string& host,
            std::uint16_t port,
            const dsbox_net_socket::ConnectOptions& options) override;

        // Makes the next connect() gather the trusted certificates anew.
        void refreshTrust();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
