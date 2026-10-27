#include <dsbox_net_tls_localca/localca_trust_provider.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <limits.h>
    #include <unistd.h>
#endif

namespace dsbox_net_tls_localca
{
    namespace
    {
        // Directory containing the running executable itself -- the same
        // convention launcher/main.cpp uses for --pathmode 0 (its
        // default), reimplemented here rather than shared. See
        // ResolveTrustedCaDirSignature's own docs for why: the project
        // has deferred a shared base-path abstraction in favor of
        // Overridable for now, so a host that resolves its base
        // directory differently (a different --pathmode, a fixed
        // deployment path, ...) overrides "net.tls.localca.resolve_dir"
        // rather than this function needing to know about that host's
        // own conventions.
        std::filesystem::path executableDirectory()
        {
#if defined(_WIN32)
            wchar_t buffer[MAX_PATH];
            const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);

            if (length == 0 || length == MAX_PATH)
                throw std::runtime_error("Failed to determine the executable's own path");

            return std::filesystem::path(buffer).parent_path();
#else
            char buffer[PATH_MAX];
            const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);

            if (length == -1)
                throw std::runtime_error(
                    "Failed to determine the executable's own path (/proc/self/exe)");

            buffer[length] = '\0';
            return std::filesystem::path(buffer).parent_path();
#endif
        }

        std::string defaultResolveTrustedCaDir(TrustedCaDirKind kind)
        {
            auto dir = executableDirectory() / "config";

            dir = (kind == TrustedCaDirKind::Default)
                ? dir / "default" / "trusted_ca"
                : dir / "trusted_ca";

            return dir.string();
        }

        std::vector<std::byte> readWholeFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);

            if (!file)
                return {};

            std::error_code sizeError;
            const auto size = std::filesystem::file_size(path, sizeError);

            std::vector<std::byte> bytes;

            if (!sizeError)
            {
                bytes.resize(static_cast<std::size_t>(size));
                file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                bytes.resize(static_cast<std::size_t>(file.gcount()));
            }
            else
            {
                // file_size() can fail for reasons that don't prevent
                // reading (some virtual filesystems); fall back to
                // reading it as a stream of unknown length.
                std::vector<char> chunk(64 * 1024);
                while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || file.gcount() > 0)
                {
                    const auto* begin = reinterpret_cast<const std::byte*>(chunk.data());
                    bytes.insert(bytes.end(), begin, begin + file.gcount());
                }
            }

            return bytes;
        }

        std::vector<CaDirectoryFile> defaultReadCaDirectory(std::string directoryPath)
        {
            namespace fs = std::filesystem;

            std::vector<CaDirectoryFile> result;

            std::error_code existsError;
            if (!fs::exists(directoryPath, existsError) || existsError)
                return result; // missing directory is not an error -- see the signature's own docs

            std::error_code iterateError;
            fs::directory_iterator iterator(directoryPath, iterateError);

            if (iterateError)
                return result;

            for (const auto& entry : iterator)
            {
                std::error_code entryError;
                if (!entry.is_regular_file(entryError) || entryError)
                    continue;

                std::string extension = entry.path().extension().string();
                for (char& c : extension)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                if (extension != ".pem" && extension != ".der" && extension != ".crt")
                    continue;

                CaDirectoryFile file;
                file.fileName = entry.path().filename().string();
                file.data = readWholeFile(entry.path());
                result.push_back(std::move(file));
            }

            return result;
        }

        constexpr std::string_view kPemBegin = "-----BEGIN CERTIFICATE-----";
        constexpr std::string_view kPemEnd = "-----END CERTIFICATE-----";

        bool looksLikePem(std::string_view text)
        {
            return text.find(kPemBegin) != std::string_view::npos;
        }

        std::vector<dsbox_net_tls::Certificate> splitPemCertificates(std::string_view text)
        {
            std::vector<dsbox_net_tls::Certificate> result;
            std::size_t pos = 0;

            while (true)
            {
                const auto begin = text.find(kPemBegin, pos);
                if (begin == std::string_view::npos)
                    break;

                const auto endMarker = text.find(kPemEnd, begin);
                if (endMarker == std::string_view::npos)
                    break; // truncated trailing block -- ignore the rest

                const auto blockEnd = endMarker + kPemEnd.size();
                std::string_view block = text.substr(begin, blockEnd - begin);

                dsbox_net_tls::Certificate cert;
                cert.encoding = dsbox_net_tls::CertificateEncoding::Pem;
                cert.data.assign(
                    reinterpret_cast<const std::byte*>(block.data()),
                    reinterpret_cast<const std::byte*>(block.data()) + block.size());

                result.push_back(std::move(cert));
                pos = blockEnd;
            }

            return result;
        }
    }

    std::vector<dsbox_net_tls::Certificate> parseCertificatesFromFileContent(
        std::span<const std::byte> fileContent)
    {
        std::string_view text(reinterpret_cast<const char*>(fileContent.data()), fileContent.size());

        if (looksLikePem(text))
            return splitPemCertificates(text);

        dsbox_net_tls::Certificate cert;
        cert.encoding = dsbox_net_tls::CertificateEncoding::Der;
        cert.data.assign(fileContent.begin(), fileContent.end());

        return { std::move(cert) };
    }

    LocalCaTrustProvider::LocalCaTrustProvider(
        dsbox::CommunicationManager& comm,
        std::string resolveDirOverrideName,
        std::string readDirOverrideName)
        : m_resolveDir(comm, std::move(resolveDirOverrideName), &defaultResolveTrustedCaDir)
        , m_readDir(comm, std::move(readDirOverrideName), &defaultReadCaDirectory)
    {
    }

    std::vector<dsbox_net_tls::Certificate> LocalCaTrustProvider::trustedCertificates() const
    {
        std::vector<dsbox_net_tls::Certificate> result;

        for (const auto kind : { TrustedCaDirKind::Default, TrustedCaDirKind::Override })
        {
            const std::string dir = m_resolveDir(kind);
            const auto files = m_readDir(dir);

            for (const auto& file : files)
            {
                auto certs = parseCertificatesFromFileContent(file.data);

                result.insert(
                    result.end(),
                    std::make_move_iterator(certs.begin()),
                    std::make_move_iterator(certs.end()));
            }
        }

        return result;
    }
}
