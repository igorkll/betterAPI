#include "test_framework.hpp"

#include <dsbox_net_tls_systemca/systemca_trust_provider.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace dsbox_net_tls_systemca_tests;
using namespace dsbox_net_tls_systemca;
using namespace dsbox_net_tls;

namespace
{
    namespace fs = std::filesystem;

    // A scratch directory removed again when the test ends.
    class TempDir
    {
    public:
        TempDir()
        {
            static std::atomic<int> counter{ 0 };

            m_path = fs::temp_directory_path()
                / ("dsbox_systemca_test_" + std::to_string(::std::rand()) + "_" + std::to_string(++counter));

            fs::create_directories(m_path);
        }

        ~TempDir()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const fs::path& path() const { return m_path; }

        std::string write(const std::string& name, const std::string& content) const
        {
            const fs::path file = m_path / name;
            std::ofstream out(file, std::ios::binary);
            out << content;
            return file.string();
        }

    private:
        fs::path m_path;
    };

    // The provider only splits on the BEGIN/END markers and never
    // parses the base64 inside, so any distinct body will do.
    std::string pemBlock(const std::string& body)
    {
        return "-----BEGIN CERTIFICATE-----\n" + body + "\n-----END CERTIFICATE-----\n";
    }

    std::string text(const Certificate& certificate)
    {
        return std::string(reinterpret_cast<const char*>(certificate.data.data()), certificate.data.size());
    }

    bool contains(const std::vector<Certificate>& certificates, const std::string& body)
    {
        for (const auto& certificate : certificates)
        {
            if (text(certificate).find(body) != std::string::npos)
                return true;
        }

        return false;
    }

    /*
     * The platform's native store
     */

    void testReadsRealSystemStoreWithoutThrowing()
    {
        // A smoke test against the real machine. On Windows every
        // install has at least a handful of built-in root CAs, so the
        // result is expected to be non-empty and DER; on other systems
        // the store may legitimately be absent (a minimal container
        // has no CA bundle), so only the shape of what is returned is
        // checked.
        SystemCaTrustProvider provider;
        auto certs = provider.trustedCertificates();

#if defined(_WIN32)
        DSBOX_NET_TLS_SYSTEMCA_CHECK(!certs.empty());
#endif

        for (const auto& cert : certs)
        {
#if defined(_WIN32)
            DSBOX_NET_TLS_SYSTEMCA_CHECK(cert.encoding == CertificateEncoding::Der);
#else
            DSBOX_NET_TLS_SYSTEMCA_CHECK(cert.encoding == CertificateEncoding::Pem);
#endif
            DSBOX_NET_TLS_SYSTEMCA_CHECK(!cert.data.empty());
        }
    }

    void testCalledTwiceProducesConsistentCount()
    {
        // Not a caching guarantee (see ITrustProvider's own docs -- this
        // class does no caching) -- just confirms the store handle
        // lifecycle (open/enumerate/close each call) doesn't leak or
        // corrupt state across repeated calls.
        SystemCaTrustProvider provider;

        const auto first = provider.trustedCertificates().size();
        const auto second = provider.trustedCertificates().size();

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(first, second);
    }

    /*
     * Explicit file sources (every platform)
     */

    void testBundleFileIsSplitIntoCertificates()
    {
        TempDir dir;

        CaFileSources sources;
        sources.bundleFiles.push_back(dir.write("bundle.pem",
            "# a comment\n"
            + pemBlock("AAAA")
            + "some text between blocks\n"
            + "-----BEGIN PRIVATE KEY-----\nSECRET\n-----END PRIVATE KEY-----\n"
            + pemBlock("BBBB")
            + pemBlock("CCCC")));

        const auto certs = SystemCaTrustProvider(sources).trustedCertificates();

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(3));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "AAAA"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "BBBB"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "CCCC"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(!contains(certs, "SECRET")); // only certificates, never keys

        for (const auto& cert : certs)
        {
            DSBOX_NET_TLS_SYSTEMCA_CHECK(cert.encoding == CertificateEncoding::Pem);
            DSBOX_NET_TLS_SYSTEMCA_CHECK(text(cert).rfind("-----BEGIN CERTIFICATE-----", 0) == 0);
        }
    }

    void testDuplicateBlocksAreReturnedOnce()
    {
        TempDir dir;

        CaFileSources sources;
        sources.bundleFiles.push_back(dir.write("bundle.pem", pemBlock("AAAA") + pemBlock("BBBB") + pemBlock("AAAA")));

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(SystemCaTrustProvider(sources).trustedCertificates().size(), static_cast<std::size_t>(2));
    }

    void testUnterminatedBlockIsIgnored()
    {
        TempDir dir;

        CaFileSources sources;
        sources.bundleFiles.push_back(dir.write("bundle.pem",
            pemBlock("AAAA") + "-----BEGIN CERTIFICATE-----\nTRUNCATED"));

        const auto certs = SystemCaTrustProvider(sources).trustedCertificates();

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "AAAA"));
    }

    void testFirstUsableBundleWins()
    {
        TempDir dir;

        CaFileSources sources;
        sources.bundleFiles.push_back((dir.path() / "does_not_exist.pem").string());
        sources.bundleFiles.push_back(dir.write("empty.pem", "no certificates in here\n"));
        sources.bundleFiles.push_back(dir.write("first.pem", pemBlock("FIRST")));
        sources.bundleFiles.push_back(dir.write("second.pem", pemBlock("SECOND")));

        const auto certs = SystemCaTrustProvider(sources).trustedCertificates();

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "FIRST"));
    }

    void testDirectoriesAreUsedWhenNoBundleYields()
    {
        TempDir bundleless;
        TempDir dirA;
        TempDir dirB;

        dirA.write("a.pem", pemBlock("AAAA"));
        dirA.write("b.CRT", pemBlock("BBBB")); // extension match is case-insensitive
        dirA.write("notes.txt", pemBlock("IGNORED"));
        dirA.write("c.cer", pemBlock("CCCC"));
        dirB.write("d.pem", pemBlock("DDDD"));
        dirB.write("a_copy.pem", pemBlock("AAAA")); // same certificate as in dirA

        CaFileSources sources;
        sources.bundleFiles.push_back((bundleless.path() / "missing.pem").string());
        sources.directories.push_back(dirA.path().string());
        sources.directories.push_back((bundleless.path() / "no_such_dir").string());
        sources.directories.push_back(dirB.path().string());

        const auto certs = SystemCaTrustProvider(sources).trustedCertificates();

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(4));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "AAAA"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "BBBB"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "CCCC"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "DDDD"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(!contains(certs, "IGNORED"));
    }

    void testBundleTakesPrecedenceOverDirectories()
    {
        TempDir dir;
        TempDir certDir;

        certDir.write("other.pem", pemBlock("FROMDIR"));

        CaFileSources sources;
        sources.bundleFiles.push_back(dir.write("bundle.pem", pemBlock("FROMBUNDLE")));
        sources.directories.push_back(certDir.path().string());

        const auto certs = SystemCaTrustProvider(sources).trustedCertificates();

        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(certs.size(), static_cast<std::size_t>(1));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(certs, "FROMBUNDLE"));
    }

    void testNothingReadableGivesAnEmptyList()
    {
        TempDir dir;

        CaFileSources sources;
        sources.bundleFiles.push_back((dir.path() / "missing.pem").string());
        sources.directories.push_back((dir.path() / "missing_dir").string());

        DSBOX_NET_TLS_SYSTEMCA_CHECK(SystemCaTrustProvider(sources).trustedCertificates().empty());
        DSBOX_NET_TLS_SYSTEMCA_CHECK(SystemCaTrustProvider(CaFileSources{}).trustedCertificates().empty());
    }

    void testSourcesAreReReadOnEveryCall()
    {
        TempDir dir;
        const std::string file = dir.write("bundle.pem", pemBlock("OLD"));

        CaFileSources sources;
        sources.bundleFiles.push_back(file);

        SystemCaTrustProvider provider(sources);
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(provider.trustedCertificates(), "OLD"));

        dir.write("bundle.pem", pemBlock("NEW"));

        const auto after = provider.trustedCertificates();
        DSBOX_NET_TLS_SYSTEMCA_CHECK(contains(after, "NEW"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(!contains(after, "OLD"));
    }

    /*
     * Default locations
     */

    void testDefaultFileSourcesListTheConventionalLocations()
    {
        const auto sources = SystemCaTrustProvider::defaultFileSources();

        const auto has = [](const std::vector<std::string>& list, const std::string& item)
        {
            return std::find(list.begin(), list.end(), item) != list.end();
        };

        DSBOX_NET_TLS_SYSTEMCA_CHECK(has(sources.bundleFiles, "/etc/ssl/certs/ca-certificates.crt"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(has(sources.bundleFiles, "/etc/pki/tls/certs/ca-bundle.crt"));
        DSBOX_NET_TLS_SYSTEMCA_CHECK(has(sources.directories, "/etc/ssl/certs"));
    }

    // Sets an environment variable for one scope, then puts it back.
    class ScopedEnvironment
    {
    public:
        ScopedEnvironment(const char* name, const std::string& value)
            : m_name(name)
        {
            if (const char* old = std::getenv(name))
            {
                m_hadOld = true;
                m_old = old;
            }

            set(value.c_str());
        }

        ~ScopedEnvironment()
        {
            if (m_hadOld)
                set(m_old.c_str());
            else
                unset();
        }

    private:
        void set(const char* value)
        {
#if defined(_WIN32)
            _putenv_s(m_name.c_str(), value);
#else
            ::setenv(m_name.c_str(), value, 1);
#endif
        }

        void unset()
        {
#if defined(_WIN32)
            _putenv_s(m_name.c_str(), "");
#else
            ::unsetenv(m_name.c_str());
#endif
        }

        std::string m_name;
        std::string m_old;
        bool m_hadOld = false;
    };

    void testEnvironmentVariablesTakePrecedence()
    {
#if defined(_WIN32)
        const std::string separator = ";";
#else
        const std::string separator = ":";
#endif

        ScopedEnvironment file("SSL_CERT_FILE", "/custom/bundle.pem");
        ScopedEnvironment directories("SSL_CERT_DIR", "/custom/dir1" + separator + "/custom/dir2");

        const auto sources = SystemCaTrustProvider::defaultFileSources();

        DSBOX_NET_TLS_SYSTEMCA_CHECK(!sources.bundleFiles.empty());
        DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(sources.bundleFiles.front(), "/custom/bundle.pem");

        DSBOX_NET_TLS_SYSTEMCA_CHECK(sources.directories.size() >= 2);

        if (sources.directories.size() >= 2)
        {
            DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(sources.directories[0], "/custom/dir1");
            DSBOX_NET_TLS_SYSTEMCA_CHECK_EQ(sources.directories[1], "/custom/dir2");
        }
    }
}

void runSystemCaTrustProviderTests()
{
    runTest("testReadsRealSystemStoreWithoutThrowing", testReadsRealSystemStoreWithoutThrowing);
    runTest("testCalledTwiceProducesConsistentCount", testCalledTwiceProducesConsistentCount);

    runTest("testBundleFileIsSplitIntoCertificates", testBundleFileIsSplitIntoCertificates);
    runTest("testDuplicateBlocksAreReturnedOnce", testDuplicateBlocksAreReturnedOnce);
    runTest("testUnterminatedBlockIsIgnored", testUnterminatedBlockIsIgnored);
    runTest("testFirstUsableBundleWins", testFirstUsableBundleWins);
    runTest("testDirectoriesAreUsedWhenNoBundleYields", testDirectoriesAreUsedWhenNoBundleYields);
    runTest("testBundleTakesPrecedenceOverDirectories", testBundleTakesPrecedenceOverDirectories);
    runTest("testNothingReadableGivesAnEmptyList", testNothingReadableGivesAnEmptyList);
    runTest("testSourcesAreReReadOnEveryCall", testSourcesAreReReadOnEveryCall);

    runTest("testDefaultFileSourcesListTheConventionalLocations", testDefaultFileSourcesListTheConventionalLocations);
    runTest("testEnvironmentVariablesTakePrecedence", testEnvironmentVariablesTakePrecedence);
}
