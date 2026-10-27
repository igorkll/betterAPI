#pragma once

// Test-only: a throwaway PKI (CA + server certificates, generated in
// memory) and a minimal TLS server session over an accepted raw socket,
// so the TLS client is tested against real OpenSSL on the other end.

#include "loopback_server.hpp"

#include <dsbox_net_tls/trust_provider.hpp>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dsbox_test_pki
{
    using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using CertPtr = std::unique_ptr<X509, decltype(&X509_free)>;

    struct Identity
    {
        KeyPtr key{ nullptr, EVP_PKEY_free };
        CertPtr cert{ nullptr, X509_free };
    };

    struct CertSpec
    {
        std::string commonName = "test";
        std::string alternativeNames;          // e.g. "DNS:localhost,IP:127.0.0.1"
        bool isCa = false;
        long notBeforeOffsetSeconds = -3600;   // relative to now
        long notAfterOffsetSeconds = 3600;
    };

    inline KeyPtr generateKey()
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);

        if (!context
            || EVP_PKEY_keygen_init(context.get()) <= 0
            || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(context.get(), NID_X9_62_prime256v1) <= 0)
        {
            throw std::runtime_error("test PKI: key generation setup failed");
        }

        EVP_PKEY* raw = nullptr;

        if (EVP_PKEY_keygen(context.get(), &raw) <= 0)
            throw std::runtime_error("test PKI: key generation failed");

        return KeyPtr(raw, EVP_PKEY_free);
    }

    inline void addExtension(X509* subject, X509* issuer, int nid, const std::string& value)
    {
        X509V3_CTX context;
        X509V3_set_ctx(&context, issuer, subject, nullptr, nullptr, 0);

        X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());

        if (!extension)
            throw std::runtime_error("test PKI: bad extension: " + value);

        X509_add_ext(subject, extension, -1);
        X509_EXTENSION_free(extension);
    }

    // `issuer` null means self-signed.
    inline Identity issue(const CertSpec& spec, const Identity* issuer = nullptr)
    {
        static long serial = 1;

        Identity identity;
        identity.key = generateKey();
        identity.cert.reset(X509_new());

        X509* x = identity.cert.get();

        X509_set_version(x, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(x), serial++);
        X509_gmtime_adj(X509_getm_notBefore(x), spec.notBeforeOffsetSeconds);
        X509_gmtime_adj(X509_getm_notAfter(x), spec.notAfterOffsetSeconds);

        X509_NAME* name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(spec.commonName.c_str()), -1, -1, 0);

        X509_set_issuer_name(x, issuer ? X509_get_subject_name(issuer->cert.get()) : name);
        X509_set_pubkey(x, identity.key.get());

        X509* issuerCert = issuer ? issuer->cert.get() : x;

        addExtension(x, issuerCert, NID_basic_constraints, spec.isCa ? "critical,CA:TRUE" : "critical,CA:FALSE");
        addExtension(x, issuerCert, NID_key_usage, spec.isCa ? "critical,keyCertSign,cRLSign" : "critical,digitalSignature");

        if (!spec.alternativeNames.empty())
            addExtension(x, issuerCert, NID_subject_alt_name, spec.alternativeNames);

        EVP_PKEY* signingKey = issuer ? issuer->key.get() : identity.key.get();

        if (X509_sign(x, signingKey, EVP_sha256()) <= 0)
            throw std::runtime_error("test PKI: signing failed");

        return identity;
    }

    inline std::vector<std::byte> toPem(X509* certificate)
    {
        std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
        PEM_write_bio_X509(bio.get(), certificate);

        char* data = nullptr;
        const long size = BIO_get_mem_data(bio.get(), &data);

        std::vector<std::byte> result(static_cast<std::size_t>(size));
        std::memcpy(result.data(), data, result.size());
        return result;
    }

    inline std::vector<std::byte> toDer(X509* certificate)
    {
        const int size = i2d_X509(certificate, nullptr);
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        unsigned char* cursor = reinterpret_cast<unsigned char*>(result.data());
        i2d_X509(certificate, &cursor);
        return result;
    }

    inline dsbox_net_tls::Certificate pemCertificate(X509* certificate)
    {
        return { dsbox_net_tls::CertificateEncoding::Pem, toPem(certificate) };
    }

    inline dsbox_net_tls::Certificate derCertificate(X509* certificate)
    {
        return { dsbox_net_tls::CertificateEncoding::Der, toDer(certificate) };
    }

    // A trust provider whose contents the test can change at any time.
    class FixedTrust final : public dsbox_net_tls::ITrustProvider
    {
    public:
        explicit FixedTrust(std::vector<dsbox_net_tls::Certificate> certificates = {})
            : m_certificates(std::move(certificates))
        {
        }

        void set(std::vector<dsbox_net_tls::Certificate> certificates)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_certificates = std::move(certificates);
        }

        std::vector<dsbox_net_tls::Certificate> trustedCertificates() const override
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_certificates;
        }

    private:
        mutable std::mutex m_mutex;
        std::vector<dsbox_net_tls::Certificate> m_certificates;
    };

    inline std::shared_ptr<SSL_CTX> makeServerContext(const Identity& identity)
    {
        std::shared_ptr<SSL_CTX> context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);

        if (!context
            || SSL_CTX_use_certificate(context.get(), identity.cert.get()) != 1
            || SSL_CTX_use_PrivateKey(context.get(), identity.key.get()) != 1)
        {
            throw std::runtime_error("test PKI: could not set up the server context");
        }

        return context;
    }

    // The server side of one TLS session, over a socket accepted by a
    // LoopbackServer.
    class ServerSession
    {
    public:
        ServerSession(SSL_CTX* context, dsbox_test_net::RawConnection& peer)
            : m_ssl(SSL_new(context), SSL_free)
        {
            SSL_set_fd(m_ssl.get(), static_cast<int>(peer.native()));
        }

        // False if the client abandoned or failed the handshake.
        bool handshake()
        {
            return SSL_accept(m_ssl.get()) == 1;
        }

        // Reads until `delimiter` has been seen, the way
        // RawConnection::readUntil does.
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

        bool send(std::string_view data)
        {
            while (!data.empty())
            {
                const int chunk = static_cast<int>(data.size() < (1u << 20) ? data.size() : (1u << 20));
                const int n = SSL_write(m_ssl.get(), data.data(), chunk);

                if (n <= 0)
                    return false;

                data.remove_prefix(static_cast<std::size_t>(n));
            }

            return true;
        }

        // Sends close_notify.
        void shutdown()
        {
            SSL_shutdown(m_ssl.get());
        }

        // Whether the peer has sent close_notify (as opposed to the
        // connection merely ending).
        bool receivedCloseNotify() const
        {
            return (SSL_get_shutdown(m_ssl.get()) & SSL_RECEIVED_SHUTDOWN) != 0;
        }

    private:
        bool fill()
        {
            char chunk[4096];
            const int n = SSL_read(m_ssl.get(), chunk, sizeof(chunk));

            if (n <= 0)
                return false;

            m_buffer.append(chunk, static_cast<std::size_t>(n));
            return true;
        }

        std::unique_ptr<SSL, decltype(&SSL_free)> m_ssl;
        std::string m_buffer;
    };
}
