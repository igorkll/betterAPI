#include <dsbox_net_file/file_protocol_handler.hpp>

#include <dsbox_net/error.hpp>
#include <dsbox_net/response.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace dsbox_net_file
{
    namespace
    {
        std::string percentDecode(std::string_view text)
        {
            auto hexValue = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';

                return std::tolower(static_cast<unsigned char>(c)) - 'a' + 10;
            };

            std::string result;
            result.reserve(text.size());

            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const bool isEscape =
                    text[i] == '%'
                    && i + 2 < text.size()
                    && std::isxdigit(static_cast<unsigned char>(text[i + 1]))
                    && std::isxdigit(static_cast<unsigned char>(text[i + 2]));

                if (isEscape)
                {
                    result += static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2]));
                    i += 2;
                }
                else
                {
                    result += text[i];
                }
            }

            return result;
        }

        // True for a decoded URL path of the form "/<letter>:" or
        // "/<letter>:/..." -- e.g. "/C:" or "/C:/Users/x". This is the
        // only Windows-path shape this module recognizes (see
        // FilePathStyle's own docs); UNC ("//server/share/...") paths
        // are out of scope for now.
        bool looksLikeWindowsDrivePath(std::string_view decodedPath)
        {
            return decodedPath.size() >= 3
                && decodedPath[0] == '/'
                && std::isalpha(static_cast<unsigned char>(decodedPath[1]))
                && decodedPath[2] == ':'
                && (decodedPath.size() == 3 || decodedPath[3] == '/');
        }

        // Precondition: looksLikeWindowsDrivePath(decodedPath).
        std::string toWindowsPath(std::string_view decodedPath)
        {
            std::string result;
            result += static_cast<char>(std::toupper(static_cast<unsigned char>(decodedPath[1])));
            result += ':';

            for (std::size_t i = 3; i < decodedPath.size(); ++i)
                result += (decodedPath[i] == '/') ? '\\' : decodedPath[i];

            if (result.size() == 2) // just the drive root, e.g. "/C:"
                result += '\\';

            return result;
        }

        // Reads a std::ifstream in chunks -- the one concrete
        // dsbox_net::IByteStream this module needs, matching
        // NetworkManager's own pull-based body model (see
        // network_manager.hpp's own design note): the underlying file
        // handle stays open only as long as something is actively
        // pulling read() from it, and the actual disk I/O happens on
        // whichever thread calls read(), not on the thread that called
        // FileProtocolHandler::start().
        class IfstreamByteStream final : public dsbox_net::IByteStream
        {
        public:
            explicit IfstreamByteStream(std::unique_ptr<std::ifstream> stream)
                : m_stream(std::move(stream))
            {
            }

            std::size_t read(std::span<std::byte> out) override
            {
                // close() may arrive from another thread (see
                // IByteStream::close()); std::ifstream is not safe for
                // that, so both go through one mutex. A local-disk read()
                // never blocks indefinitely, so close() waiting for an
                // in-progress one is fine.
                std::lock_guard<std::mutex> lock(m_mutex);

                if (!m_stream || !m_stream->is_open() || out.empty())
                    return 0;

                m_stream->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));

                if (m_stream->bad())
                {
                    throw dsbox_net::NetException(dsbox_net::NetError{
                        dsbox_net::NetErrorCode::Internal,
                        "I/O error while reading a local file",
                        0
                    });
                }

                return static_cast<std::size_t>(m_stream->gcount());
            }

            void close() noexcept override
            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (m_stream && m_stream->is_open())
                    m_stream->close();
            }

        private:
            std::mutex m_mutex;
            std::unique_ptr<std::ifstream> m_stream;
        };

        // The real filesystem access behind the "net.file.open"
        // Overridable's fallback -- see FileModule's own docs for how a
        // host/test replaces this entirely.
        FileOpenResult defaultFileOpen(std::string nativePath)
        {
            namespace fs = std::filesystem;

            FileOpenResult result;
            std::error_code existsError;

            if (!fs::exists(nativePath, existsError) || existsError)
            {
                result.failure = FileOpenResult::Failure::NotFound;
                result.message = "No such file: " + nativePath;
                return result;
            }

            std::error_code directoryError;
            if (fs::is_directory(nativePath, directoryError) && !directoryError)
            {
                result.failure = FileOpenResult::Failure::Other;
                result.message = "Refusing to open a directory as a file: " + nativePath;
                return result;
            }

            auto stream = std::make_unique<std::ifstream>(nativePath, std::ios::binary);

            if (!stream->is_open())
            {
                // fs::exists() already confirmed the path is there, so a
                // failure to open it now is most likely a permissions
                // problem rather than a missing file -- std::ifstream
                // itself doesn't reliably distinguish the two causes
                // across platforms.
                result.failure = FileOpenResult::Failure::Denied;
                result.message = "Could not open: " + nativePath;
                return result;
            }

            std::error_code sizeError;
            const auto size = fs::file_size(nativePath, sizeError);

            if (!sizeError)
                result.size = static_cast<std::uint64_t>(size);

            result.stream = std::make_unique<IfstreamByteStream>(std::move(stream));
            return result;
        }
    }

    std::optional<std::string> nativePathFromUrlPath(std::string_view urlPath, FilePathStyle style)
    {
        const std::string decoded = percentDecode(urlPath);

        if (decoded.empty() || decoded.front() != '/')
            return std::nullopt;

        // An embedded NUL (from "%00") would silently truncate the path
        // in C-string based filesystem APIs.
        if (decoded.find('\0') != std::string::npos)
            return std::nullopt;

        const bool isWindowsDrive = looksLikeWindowsDrivePath(decoded);

        switch (style)
        {
            case FilePathStyle::Default:
                return std::nullopt; // misuse -- see nativePathFromUrlPath's own docs

            case FilePathStyle::PosixOnly:
                return decoded;

            case FilePathStyle::WindowsOnly:
                return isWindowsDrive ? std::make_optional(toWindowsPath(decoded)) : std::nullopt;

            case FilePathStyle::Both:
                return isWindowsDrive ? toWindowsPath(decoded) : decoded;
        }

        return std::nullopt; // unreachable, silences a compiler warning
    }

    FileProtocolHandler::FileProtocolHandler(
        dsbox::CommunicationManager& comm,
        std::string overrideName,
        FilePathStyle pathStyle)
        : m_open(comm, std::move(overrideName), &defaultFileOpen)
        , m_pathStyle(pathStyle)
    {
        if (pathStyle == FilePathStyle::Default)
        {
            throw std::invalid_argument(
                "FileProtocolHandler: FilePathStyle::Default must be resolved by the caller "
                "(FileModule::onInit() does this) -- this class has no configuration to resolve it against");
        }
    }

    void FileProtocolHandler::start(dsbox_net::Request request, dsbox_net::FetchCallback done)
    {
        // file://host/path names a file on ANOTHER machine. Only an empty
        // host ("file:///...") or "localhost" means "this machine"; quietly
        // reading the local path of a remote-looking URL would be the
        // wrong file at best.
        if (!request.url.host.empty() && request.url.host != "localhost")
        {
            done(dsbox_net::FetchResult{
                dsbox_net::NetError{
                    dsbox_net::NetErrorCode::UnsupportedOperation,
                    "file:// URLs with a remote host are not supported",
                    0
                },
                {}
            });
            return;
        }

        const auto nativePath = nativePathFromUrlPath(request.url.path, m_pathStyle);

        if (!nativePath)
        {
            dsbox_net::FetchResult result;
            result.response.url = request.url;
            result.response.statusClass = dsbox_net::StatusClass::NotFound;
            result.response.statusText =
                "URL path does not match the configured FilePathStyle for the file:// scheme";
            done(std::move(result));
            return;
        }

        FileOpenResult opened;

        try
        {
            opened = m_open(*nativePath);
        }
        catch (const std::exception& exception)
        {
            done(dsbox_net::FetchResult{
                dsbox_net::NetError{ dsbox_net::NetErrorCode::Internal, exception.what(), 0 },
                {}
            });
            return;
        }

        dsbox_net::FetchResult result;
        result.response.url = request.url;

        switch (opened.failure)
        {
            case FileOpenResult::Failure::None:
                result.response.status = 200;
                result.response.statusClass = dsbox_net::StatusClass::Success;

                if (opened.size.has_value())
                    result.response.metadata.set("Content-Length", std::to_string(*opened.size));

                result.response.body = std::move(opened.stream);
                break;

            case FileOpenResult::Failure::NotFound:
                result.response.statusClass = dsbox_net::StatusClass::NotFound;
                result.response.statusText = opened.message;
                break;

            case FileOpenResult::Failure::Denied:
                result.response.statusClass = dsbox_net::StatusClass::Denied;
                result.response.statusText = opened.message;
                break;

            case FileOpenResult::Failure::Other:
                done(dsbox_net::FetchResult{
                    dsbox_net::NetError{ dsbox_net::NetErrorCode::Internal, opened.message, 0 },
                    {}
                });
                return;
        }

        done(std::move(result));
    }
}
