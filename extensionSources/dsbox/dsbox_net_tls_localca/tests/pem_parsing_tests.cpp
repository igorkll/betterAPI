#include "test_framework.hpp"

#include <dsbox_net_tls_localca/localca_trust_provider.hpp>

using namespace dsbox_net_tls_localca_tests;
using namespace dsbox_net_tls_localca;
using namespace dsbox_net_tls;

namespace
{
    std::span<const std::byte> toSpan(const std::string& text)
    {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()),
            text.size());
    }

    std::string toText(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    const std::string kCertA =
        "-----BEGIN CERTIFICATE-----\n"
        "AAAAAAAAAAAAAAAA\n"
        "-----END CERTIFICATE-----\n";

    const std::string kCertB =
        "-----BEGIN CERTIFICATE-----\n"
        "BBBBBBBBBBBBBBBB\n"
        "-----END CERTIFICATE-----\n";

    void testSinglePemCertificate()
    {
        auto certs = parseCertificatesFromFileContent(toSpan(kCertA));

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_LOCALCA_CHECK(certs[0].encoding == CertificateEncoding::Pem);
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(toText(certs[0].data), kCertA);
    }

    void testPemBundleSplitsIntoSeveralCertificates()
    {
        const std::string bundle = kCertA + kCertB;
        auto certs = parseCertificatesFromFileContent(toSpan(bundle));

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(2));
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(toText(certs[0].data), kCertA);
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(toText(certs[1].data), kCertB);
    }

    void testNonPemContentIsTreatedAsDer()
    {
        const std::string binary = "\x30\x82\x01\x00garbage-der-bytes";
        auto certs = parseCertificatesFromFileContent(toSpan(binary));

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_LOCALCA_CHECK(certs[0].encoding == CertificateEncoding::Der);
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(toText(certs[0].data), binary);
    }

    void testOtherPemBlockTypesAreSkipped()
    {
        const std::string withKey =
            "-----BEGIN PRIVATE KEY-----\n"
            "shouldnotappear\n"
            "-----END PRIVATE KEY-----\n"
            + kCertA;

        auto certs = parseCertificatesFromFileContent(toSpan(withKey));

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(toText(certs[0].data), kCertA);
    }

    void testTruncatedTrailingBlockIsIgnored()
    {
        const std::string truncated = kCertA + "-----BEGIN CERTIFICATE-----\nno end marker";
        auto certs = parseCertificatesFromFileContent(toSpan(truncated));

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(toText(certs[0].data), kCertA);
    }

    void testEmptyContentYieldsOneEmptyDerCertificate()
    {
        // Edge case, not a realistic input: an empty file has no PEM
        // marker, so it falls through to "treat as DER" -- documenting
        // this rather than special-casing it, since a genuinely empty
        // cert file is a caller/deployment mistake either way.
        auto certs = parseCertificatesFromFileContent(std::span<const std::byte>{});

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_LOCALCA_CHECK(certs[0].encoding == CertificateEncoding::Der);
        DSBOX_NET_TLS_LOCALCA_CHECK(certs[0].data.empty());
    }
}

void runPemParsingTests()
{
    runTest("testSinglePemCertificate", testSinglePemCertificate);
    runTest("testPemBundleSplitsIntoSeveralCertificates", testPemBundleSplitsIntoSeveralCertificates);
    runTest("testNonPemContentIsTreatedAsDer", testNonPemContentIsTreatedAsDer);
    runTest("testOtherPemBlockTypesAreSkipped", testOtherPemBlockTypesAreSkipped);
    runTest("testTruncatedTrailingBlockIsIgnored", testTruncatedTrailingBlockIsIgnored);
    runTest("testEmptyContentYieldsOneEmptyDerCertificate", testEmptyContentYieldsOneEmptyDerCertificate);
}
