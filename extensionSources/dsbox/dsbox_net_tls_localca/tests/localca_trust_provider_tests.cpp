#include "test_framework.hpp"

#include <dsbox_net_tls_localca/localca_trust_provider.hpp>

#include <dsbox/communication_manager.hpp>

#include <filesystem>
#include <fstream>
#include <random>

using namespace dsbox_net_tls_localca_tests;
using namespace dsbox_net_tls_localca;
using namespace dsbox_net_tls;

namespace
{
    std::vector<std::byte> toBytes(const std::string& text)
    {
        return std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data()) + text.size());
    }

    void testBothDirectoriesAreResolvedAndAggregated()
    {
        dsbox::CommunicationManager comm;
        std::vector<TrustedCaDirKind> resolvedKinds;

        comm.registerHandler<ResolveTrustedCaDirSignature>(
            "test.resolve",
            [&](TrustedCaDirKind kind) -> std::string
            {
                resolvedKinds.push_back(kind);
                return kind == TrustedCaDirKind::Default ? "fake-default-dir" : "fake-override-dir";
            });

        comm.registerHandler<ReadCaDirectorySignature>(
            "test.read",
            [](std::string dir) -> std::vector<CaDirectoryFile>
            {
                CaDirectoryFile file;
                file.fileName = dir + "/one.pem";
                file.data = toBytes(
                    "-----BEGIN CERTIFICATE-----\nX\n-----END CERTIFICATE-----\n");
                return { file };
            });

        LocalCaTrustProvider provider(comm, "test.resolve", "test.read");
        auto certs = provider.trustedCertificates();

        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(resolvedKinds.size(), static_cast<std::size_t>(2));
        DSBOX_NET_TLS_LOCALCA_CHECK(resolvedKinds[0] == TrustedCaDirKind::Default);
        DSBOX_NET_TLS_LOCALCA_CHECK(resolvedKinds[1] == TrustedCaDirKind::Override);

        // One certificate from each of the two directories.
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(2));
    }

    void testMissingDirectoryContributesNothing()
    {
        dsbox::CommunicationManager comm;

        comm.registerHandler<ResolveTrustedCaDirSignature>(
            "test.resolve",
            [](TrustedCaDirKind) -> std::string { return "does-not-matter"; });

        comm.registerHandler<ReadCaDirectorySignature>(
            "test.read",
            [](std::string) -> std::vector<CaDirectoryFile> { return {}; }); // both "missing"

        LocalCaTrustProvider provider(comm, "test.resolve", "test.read");
        auto certs = provider.trustedCertificates();

        DSBOX_NET_TLS_LOCALCA_CHECK(certs.empty());
    }

    void testDefaultOverridesReadRealTemporaryDirectory()
    {
        // No overrides registered -- exercises the real
        // defaultResolveTrustedCaDir()/defaultReadCaDirectory() fallback
        // end to end, by overriding ONLY resolveDir (to point at a real
        // temp directory instead of "<exe dir>/config/...") and letting
        // readDir hit the actual filesystem.
        auto tempDir = std::filesystem::temp_directory_path()
            / ("dsbox_net_tls_localca_test_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(tempDir);

        {
            std::ofstream pemFile(tempDir / "root.pem", std::ios::binary);
            pemFile << "-----BEGIN CERTIFICATE-----\nY\n-----END CERTIFICATE-----\n";
        }
        {
            std::ofstream ignoredFile(tempDir / "readme.txt", std::ios::binary);
            ignoredFile << "not a certificate";
        }

        dsbox::CommunicationManager comm;

        comm.registerHandler<ResolveTrustedCaDirSignature>(
            "test.resolve",
            [&](TrustedCaDirKind kind) -> std::string
            {
                // Both kinds point at the same directory for this test --
                // only the real defaultReadCaDirectory() is under test
                // here, not the two-directory aggregation (already
                // covered above).
                (void)kind;
                return tempDir.string();
            });

        LocalCaTrustProvider provider(comm, "test.resolve", "net.tls.localca.read_dir_unused_here");
        auto certs = provider.trustedCertificates();

        // One .pem certificate, found twice (once per directory kind,
        // both resolved to the same real directory) -- readme.txt is
        // correctly skipped by extension.
        DSBOX_NET_TLS_LOCALCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(2));

        std::filesystem::remove_all(tempDir);
    }
}

void runLocalCaTrustProviderTests()
{
    runTest("testBothDirectoriesAreResolvedAndAggregated", testBothDirectoriesAreResolvedAndAggregated);
    runTest("testMissingDirectoryContributesNothing", testMissingDirectoryContributesNothing);
    runTest("testDefaultOverridesReadRealTemporaryDirectory", testDefaultOverridesReadRealTemporaryDirectory);
}
