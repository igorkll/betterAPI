#pragma once

#include <cstddef>
#include <vector>

namespace dsbox_net_tls
{
    enum class CertificateEncoding
    {
        Pem,
        Der,
    };

    // Exactly one certificate, in its original encoding, exactly as a
    // dsbox_net_tls::ITrustProvider produced it. A provider that reads a
    // PEM bundle file containing several certificates splits it into one
    // Certificate per entry itself -- nothing downstream (TrustManager,
    // and eventually whatever builds an OpenSSL X509_STORE out of this)
    // ever has to parse bundle boundaries or tell PEM from DER.
    //
    // For Pem, `data` is the certificate's PEM text including its
    // "-----BEGIN CERTIFICATE-----"/"-----END CERTIFICATE-----" markers
    // (what every PEM-consuming API, OpenSSL's PEM_read_bio_X509
    // included, expects to see). For Der, `data` is the raw DER bytes of
    // a single certificate.
    struct Certificate
    {
        CertificateEncoding encoding = CertificateEncoding::Pem;
        std::vector<std::byte> data;
    };
}
