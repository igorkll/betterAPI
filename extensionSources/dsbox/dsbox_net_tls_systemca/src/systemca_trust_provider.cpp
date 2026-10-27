#include <dsbox_net_tls_systemca/systemca_trust_provider.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <wincrypt.h>
    #pragma comment(lib, "crypt32.lib")
#endif

namespace dsbox_net_tls_systemca
{
    namespace
    {
        namespace fs = std::filesystem;

        // A CA file larger than this is not a CA file.
        constexpr std::uintmax_t kMaxFileBytes = 64u * 1024u * 1024u;

        std::optional<std::string> environmentVariable(const char* name)
        {
#if defined(_WIN32)
            char* value = nullptr;
            std::size_t length = 0;

            if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
                return std::nullopt;

            std::string result(value);
            std::free(value);
            return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
#else
            const char* value = std::getenv(name);

            if (value == nullptr || *value == '\0')
                return std::nullopt;

            return std::string(value);
#endif
        }

        bool readWholeFile(const fs::path& path, std::string& out)
        {
            std::error_code error;

            // is_regular_file follows symlinks, which matters: the
            // entries of /etc/ssl/certs are mostly links.
            if (!fs::is_regular_file(path, error) || error)
                return false;

            const auto size = fs::file_size(path, error);

            if (error || size > kMaxFileBytes)
                return false;

            std::ifstream stream(path, std::ios::binary);

            if (!stream)
                return false;

            out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
            return true;
        }

        // Appends one PEM Certificate per "BEGIN CERTIFICATE" block in
        // `text` to `out`, skipping any block already in `seen`. Other
        // block types (keys, "TRUSTED CERTIFICATE", ...) are ignored.
        void collectPemCertificates(
            const std::string& text,
            std::set<std::string>& seen,
            std::vector<dsbox_net_tls::Certificate>& out)
        {
            constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
            constexpr std::string_view kEnd = "-----END CERTIFICATE-----";

            std::size_t position = 0;

            while ((position = text.find(kBegin, position)) != std::string::npos)
            {
                std::size_t end = text.find(kEnd, position);

                if (end == std::string::npos)
                    break;

                end += kEnd.size();

                std::string block = text.substr(position, end - position);
                block += '\n';
                position = end;

                if (!seen.insert(block).second)
                    continue;

                dsbox_net_tls::Certificate certificate;
                certificate.encoding = dsbox_net_tls::CertificateEncoding::Pem;
                certificate.data.assign(
                    reinterpret_cast<const std::byte*>(block.data()),
                    reinterpret_cast<const std::byte*>(block.data()) + block.size());

                out.push_back(std::move(certificate));
            }
        }

        bool hasCertificateExtension(const fs::path& path)
        {
            std::string extension = path.extension().string();

            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });

            return extension == ".pem" || extension == ".crt" || extension == ".cer";
        }

        std::vector<dsbox_net_tls::Certificate> readFileSources(const CaFileSources& sources)
        {
            std::vector<dsbox_net_tls::Certificate> result;
            std::set<std::string> seen;

            for (const std::string& file : sources.bundleFiles)
            {
                std::string text;

                if (!readWholeFile(file, text))
                    continue;

                collectPemCertificates(text, seen, result);

                if (!result.empty())
                    return result; // the first usable bundle is THE bundle
            }

            for (const std::string& directory : sources.directories)
            {
                std::error_code error;
                std::vector<fs::path> files;

                for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
                {
                    if (hasCertificateExtension(it->path()))
                        files.push_back(it->path());
                }

                // A stable order, so repeated calls give the same list.
                std::sort(files.begin(), files.end());

                for (const fs::path& file : files)
                {
                    std::string text;

                    if (readWholeFile(file, text))
                        collectPemCertificates(text, seen, result);
                }
            }

            return result;
        }

        std::vector<std::string> splitPathList(const std::string& list)
        {
#if defined(_WIN32)
            constexpr char kSeparator = ';';
#else
            constexpr char kSeparator = ':';
#endif
            std::vector<std::string> parts;
            std::size_t start = 0;

            while (start <= list.size())
            {
                const std::size_t stop = list.find(kSeparator, start);
                const std::string part = list.substr(start, stop == std::string::npos ? std::string::npos : stop - start);

                if (!part.empty())
                    parts.push_back(part);

                if (stop == std::string::npos)
                    break;

                start = stop + 1;
            }

            return parts;
        }

#if defined(_WIN32)

        std::vector<dsbox_net_tls::Certificate> readNativeStore()
        {
            std::vector<dsbox_net_tls::Certificate> result;

            HCERTSTORE store = ::CertOpenStore(
                CERT_STORE_PROV_SYSTEM_W,
                0,
                static_cast<HCRYPTPROV_LEGACY>(0),
                CERT_SYSTEM_STORE_CURRENT_USER,
                L"ROOT");

            if (!store)
                return result; // no store -- nothing trusted from this provider, not an error

            PCCERT_CONTEXT context = nullptr;

            while ((context = ::CertEnumCertificatesInStore(store, context)) != nullptr)
            {
                dsbox_net_tls::Certificate cert;
                cert.encoding = dsbox_net_tls::CertificateEncoding::Der;
                cert.data.assign(
                    reinterpret_cast<const std::byte*>(context->pbCertEncoded),
                    reinterpret_cast<const std::byte*>(context->pbCertEncoded) + context->cbCertEncoded);

                result.push_back(std::move(cert));
            }

            ::CertCloseStore(store, 0);
            return result;
        }

#else

        std::vector<dsbox_net_tls::Certificate> readNativeStore()
        {
            return readFileSources(SystemCaTrustProvider::defaultFileSources());
        }

#endif
    }

    SystemCaTrustProvider::SystemCaTrustProvider() = default;

    SystemCaTrustProvider::SystemCaTrustProvider(CaFileSources sources)
        : m_sources(std::move(sources))
    {
    }

    std::vector<dsbox_net_tls::Certificate> SystemCaTrustProvider::trustedCertificates() const
    {
        if (m_sources)
            return readFileSources(*m_sources);

        return readNativeStore();
    }

    CaFileSources SystemCaTrustProvider::defaultFileSources()
    {
        CaFileSources sources;

        // The environment variables OpenSSL itself honours come first,
        // so an administrator's choice wins over the guesses below.
        if (const auto file = environmentVariable("SSL_CERT_FILE"))
            sources.bundleFiles.push_back(*file);

        if (const auto directories = environmentVariable("SSL_CERT_DIR"))
        {
            for (std::string& directory : splitPathList(*directories))
                sources.directories.push_back(std::move(directory));
        }

        for (const char* bundle : {
            "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Alpine, Arch
            "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL, CentOS
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // newer Fedora, RHEL
            "/etc/ssl/ca-bundle.pem",                             // openSUSE
            "/etc/pki/tls/cacert.pem",                            // OpenELEC
            "/etc/ssl/cert.pem",                                  // macOS, Alpine, OpenBSD
            "/usr/local/share/certs/ca-root-nss.crt",             // FreeBSD
            "/usr/local/etc/ssl/cert.pem",                        // FreeBSD, other
        })
        {
            sources.bundleFiles.push_back(bundle);
        }

        for (const char* directory : {
            "/etc/ssl/certs",
            "/etc/pki/tls/certs",
            "/usr/local/share/certs",
        })
        {
            sources.directories.push_back(directory);
        }

        return sources;
    }
}
