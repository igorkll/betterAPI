#pragma once

#include <dsbox_net_tls_systemca/api.hpp>

#include <dsbox_net_tls/trust_provider.hpp>

#include <optional>
#include <string>
#include <vector>

namespace dsbox_net_tls_systemca
{
    // Where to look for PEM-encoded CA certificates on systems that keep
    // them in files (Linux, the BSDs, macOS' /etc/ssl/cert.pem).
    struct CaFileSources
    {
        // Single bundle files, each holding many certificates. Tried in
        // order; the FIRST one that is readable and yields at least one
        // certificate is used and the rest are ignored (bundles on one
        // machine typically hold the same certificates, and merging
        // them would only duplicate).
        std::vector<std::string> bundleFiles;

        // Directories of individual certificate files (*.pem, *.crt,
        // *.cer; not searched recursively), consulted only if no
        // bundle file yielded anything. Directories on this list are
        // all read and their certificates combined, without duplicates.
        std::vector<std::string> directories;
    };

    // Reads the operating system's trusted root certificates.
    //
    // Windows: the CryptoAPI "ROOT" store of the current user
    //   (CERT_SYSTEM_STORE_CURRENT_USER). Certificates are returned DER
    //   encoded. NOTE: Windows adds many publicly trusted roots to this
    //   store lazily, when it first needs them to validate a chain, so
    //   a fresh machine's store can lack roots a browser would accept.
    //   For production use, complement this with a bundled CA set via
    //   dsbox_net_tls_localca.
    //
    // Linux, BSDs, macOS: the distribution's CA bundle file or
    //   directory, found via CaFileSources::defaultFileSources() --
    //   SSL_CERT_FILE / SSL_CERT_DIR if set, then the conventional
    //   locations (/etc/ssl/certs/ca-certificates.crt,
    //   /etc/pki/tls/certs/ca-bundle.crt, /etc/ssl/cert.pem, ...).
    //   Certificates are returned PEM encoded. macOS' Keychain itself
    //   is not read, only the file the OS ships; user-added Keychain
    //   roots are not seen.
    //
    // If no source can be read, trustedCertificates() returns an empty
    // list -- never a "trust everything" fallback. The result is
    // simply that nothing from this provider is trusted.
    //
    // Constructed with explicit CaFileSources, the provider reads
    // exactly those files, on every platform, instead of the native
    // store -- for a deployment that wants to point at its own CA files
    // and for tests.
    class DSBOX_NET_TLS_SYSTEMCA_API SystemCaTrustProvider final : public dsbox_net_tls::ITrustProvider
    {
    public:
        SystemCaTrustProvider();
        explicit SystemCaTrustProvider(CaFileSources sources);

        std::vector<dsbox_net_tls::Certificate> trustedCertificates() const override;

        // The conventional CA file locations of Unix-like systems, with
        // SSL_CERT_FILE and SSL_CERT_DIR (if set) taking precedence.
        // Pure data: nothing is read here. Meaningful on any platform,
        // used by default only where there is no native store.
        static CaFileSources defaultFileSources();

    private:
        std::optional<CaFileSources> m_sources; // empty = the platform's native store
    };
}
