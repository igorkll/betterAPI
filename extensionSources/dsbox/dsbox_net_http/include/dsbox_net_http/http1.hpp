#pragma once

// HTTP/1.x wire format: parsing responses, framing their bodies,
// decoding chunked transfer coding, and serialising requests. Pure
// functions and small state machines over bytes -- no sockets, no
// threads, no dsbox_net_http types -- so each piece is unit-testable on
// its own and reusable by anything that speaks HTTP/1.x.
//
// Header-only on purpose: everything here is plain data plus small
// inline code, so it needs no export macro to be shared between the
// handler and the tests.
//
// References: RFC 9110 (semantics) and RFC 9112 (HTTP/1.1 messages).

#include <dsbox_net/error.hpp>
#include <dsbox_net/metadata.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dsbox_net_http::http1
{
    /*
     * Character classes and small string helpers
     */

    inline bool isAlphaNumeric(unsigned char c) noexcept
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    // RFC 9110 "tchar": what a header name or a method may consist of.
    inline bool isTokenChar(unsigned char c) noexcept
    {
        if (isAlphaNumeric(c))
            return true;

        switch (c)
        {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
        }
    }

    inline bool isToken(std::string_view text) noexcept
    {
        if (text.empty())
            return false;

        return std::all_of(text.begin(), text.end(), [](char c)
        {
            return isTokenChar(static_cast<unsigned char>(c));
        });
    }

    inline char toLowerAscii(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    inline bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
            return false;

        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (toLowerAscii(a[i]) != toLowerAscii(b[i]))
                return false;
        }

        return true;
    }

    // Trims spaces and horizontal tabs ("optional whitespace").
    inline std::string_view trimWhitespace(std::string_view text) noexcept
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
            text.remove_prefix(1);

        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.remove_suffix(1);

        return text;
    }

    // Visits each element of a comma-separated header value (trimmed,
    // empty elements skipped).
    template<typename F>
    void forEachListElement(std::string_view value, F&& visit)
    {
        while (!value.empty())
        {
            const auto comma = value.find(',');
            const std::string_view element = trimWhitespace(value.substr(0, comma));

            if (!element.empty())
                visit(element);

            if (comma == std::string_view::npos)
                break;

            value.remove_prefix(comma + 1);
        }
    }

    // True if any header called `name` lists `token` among its
    // comma-separated elements (case-insensitive) -- how
    // "Connection: keep-alive, Upgrade" is read.
    inline bool headerListContains(const dsbox_net::Metadata& headers, std::string_view name, std::string_view token)
    {
        for (const auto value : headers.getAll(name))
        {
            bool found = false;

            forEachListElement(value, [&](std::string_view element)
            {
                if (equalsIgnoreCase(element, token))
                    found = true;
            });

            if (found)
                return true;
        }

        return false;
    }

    /*
     * Response head
     */

    struct ResponseHead
    {
        int status = 0;
        std::string reason;
        int minorVersion = 1; // HTTP/1.<minorVersion>
        dsbox_net::Metadata headers;
    };

    struct HeadLimits
    {
        std::size_t maxHeadBytes = 64 * 1024; // status line + all headers
        std::size_t maxHeaderCount = 200;
    };

    // Incrementally parses a response's status line and headers. Feed it
    // bytes as they arrive; it reports Complete as soon as the blank line
    // ending the head has been seen and tells you how many of the bytes
    // you just gave it belonged to the head -- the rest are the start of
    // the body.
    class ResponseHeadParser
    {
    public:
        enum class Status
        {
            NeedMore,
            Complete,
            Error,
        };

        explicit ResponseHeadParser(HeadLimits limits = {})
            : m_limits(limits)
        {
        }

        // `consumed` is how many leading bytes of `data` were used. On
        // NeedMore and Error that is all of them; on Complete it is only
        // up to the end of the head.
        Status feed(std::span<const std::byte> data, std::size_t& consumed)
        {
            consumed = 0;

            if (m_status != Status::NeedMore)
                return m_status;

            const std::size_t previousSize = m_buffer.size();
            m_buffer.append(reinterpret_cast<const char*>(data.data()), data.size());

            const std::size_t end = findEndOfHead();

            if (end == std::string::npos)
            {
                consumed = data.size();

                if (m_buffer.size() > m_limits.maxHeadBytes)
                    return fail("response header section too large");

                return Status::NeedMore;
            }

            if (end > m_limits.maxHeadBytes)
            {
                consumed = data.size();
                return fail("response header section too large");
            }

            // The head could not have been complete before this feed
            // (we would have returned then), so end > previousSize.
            consumed = end - previousSize;
            m_buffer.resize(end);

            return parse() ? (m_status = Status::Complete) : m_status;
        }

        const ResponseHead& head() const noexcept
        {
            return m_head;
        }

        const std::string& error() const noexcept
        {
            return m_error;
        }

        void reset()
        {
            m_buffer.clear();
            m_scanPosition = 0;
            m_status = Status::NeedMore;
            m_head = {};
            m_error.clear();
        }

    private:
        Status fail(std::string message)
        {
            m_error = std::move(message);
            m_status = Status::Error;
            return m_status;
        }

        // Index one past the blank line that ends the head, or npos.
        // Tolerates bare-LF line endings, as RFC 9112 allows a recipient
        // to.
        std::size_t findEndOfHead()
        {
            for (std::size_t i = m_scanPosition; i < m_buffer.size(); ++i)
            {
                if (m_buffer[i] != '\n')
                    continue;

                if (i + 1 < m_buffer.size() && m_buffer[i + 1] == '\n')
                    return i + 2;

                if (i + 2 < m_buffer.size() && m_buffer[i + 1] == '\r' && m_buffer[i + 2] == '\n')
                    return i + 3;

                if (i + 2 >= m_buffer.size())
                {
                    // Not enough bytes after this newline to tell yet;
                    // resume from here next time.
                    m_scanPosition = i;
                    return std::string::npos;
                }
            }

            m_scanPosition = m_buffer.size();
            return std::string::npos;
        }

        bool parseStatusLine(std::string_view line)
        {
            // "HTTP/1.x SSS" optionally followed by " reason".
            if (line.size() < 12 || line.substr(0, 5) != "HTTP/")
                return fail("malformed status line"), false;

            if (line[5] != '1' || line[6] != '.' || line[7] < '0' || line[7] > '9')
                return fail("unsupported HTTP version"), false;

            if (line[8] != ' ')
                return fail("malformed status line"), false;

            for (int i = 9; i < 12; ++i)
            {
                if (line[i] < '0' || line[i] > '9')
                    return fail("malformed status code"), false;
            }

            m_head.minorVersion = line[7] - '0';
            m_head.status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');

            if (m_head.status < 100)
                return fail("status code out of range"), false;

            if (line.size() > 12)
            {
                if (line[12] != ' ')
                    return fail("malformed status line"), false;

                m_head.reason = std::string(line.substr(13));
            }

            return true;
        }

        bool parse()
        {
            std::string_view text(m_buffer);
            std::vector<std::pair<std::string, std::string>> fields;
            bool first = true;

            while (!text.empty())
            {
                const auto newline = text.find('\n');
                std::string_view line = text.substr(0, newline);
                text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);

                if (first)
                {
                    first = false;

                    if (!parseStatusLine(line))
                        return false;

                    continue;
                }

                if (line.empty())
                    break; // the blank line that ends the head

                if (line.find('\0') != std::string_view::npos || line.find('\r') != std::string_view::npos)
                    return fail("invalid character in header"), false;

                if (line.front() == ' ' || line.front() == '\t')
                {
                    // Obsolete line folding: RFC 9112 says to treat it
                    // as a single space.
                    if (fields.empty())
                        return fail("header continuation with no header"), false;

                    fields.back().second += ' ';
                    fields.back().second += trimWhitespace(line);
                    continue;
                }

                const auto colon = line.find(':');

                if (colon == std::string_view::npos || !isToken(line.substr(0, colon)))
                    return fail("malformed header field"), false;

                if (fields.size() >= m_limits.maxHeaderCount)
                    return fail("too many response headers"), false;

                fields.emplace_back(std::string(line.substr(0, colon)), std::string(trimWhitespace(line.substr(colon + 1))));
            }

            for (auto& [name, value] : fields)
                m_head.headers.add(std::move(name), std::move(value));

            return true;
        }

        HeadLimits m_limits;
        std::string m_buffer;
        std::size_t m_scanPosition = 0;
        Status m_status = Status::NeedMore;
        ResponseHead m_head;
        std::string m_error;
    };

    /*
     * Body framing
     */

    enum class FramingKind
    {
        None,          // no body at all
        ContentLength, // exactly `length` bytes
        Chunked,       // chunked transfer coding
        UntilClose,    // everything until the peer closes the connection
    };

    struct BodyFraming
    {
        FramingKind kind = FramingKind::None;
        std::uint64_t length = 0;
    };

    // How the body of a response to a `method` request is delimited
    // (RFC 9112 section 6.3). Returns nullopt, with `error` set, if the
    // headers contradict each other or are malformed -- the caller must
    // then not trust the connection.
    inline std::optional<BodyFraming> determineFraming(
        std::string_view method,
        const ResponseHead& head,
        std::string& error)
    {
        if (equalsIgnoreCase(method, "HEAD")
            || (head.status >= 100 && head.status < 200)
            || head.status == 204
            || head.status == 304)
        {
            return BodyFraming{ FramingKind::None, 0 };
        }

        if (head.headers.has("Transfer-Encoding"))
        {
            // The final coding decides; anything but "chunked" last
            // leaves us reading until close.
            std::string last;

            for (const auto value : head.headers.getAll("Transfer-Encoding"))
            {
                forEachListElement(value, [&](std::string_view element)
                {
                    last.assign(element);
                });
            }

            std::transform(last.begin(), last.end(), last.begin(), toLowerAscii);

            if (last == "chunked")
                return BodyFraming{ FramingKind::Chunked, 0 };

            return BodyFraming{ FramingKind::UntilClose, 0 };
        }

        std::optional<std::uint64_t> length;

        for (const auto value : head.headers.getAll("Content-Length"))
        {
            bool malformed = false;

            forEachListElement(value, [&](std::string_view element)
            {
                std::uint64_t parsed = 0;
                const auto result = std::from_chars(element.data(), element.data() + element.size(), parsed);

                // from_chars would also accept a sign; a Content-Length
                // is digits only.
                const bool digitsOnly = std::all_of(element.begin(), element.end(), [](char c)
                {
                    return c >= '0' && c <= '9';
                });

                if (!digitsOnly || result.ec != std::errc{} || result.ptr != element.data() + element.size())
                {
                    malformed = true;
                    return;
                }

                if (length && *length != parsed)
                    malformed = true;

                length = parsed;
            });

            if (malformed)
            {
                error = "invalid or conflicting Content-Length";
                return std::nullopt;
            }
        }

        if (length)
            return BodyFraming{ FramingKind::ContentLength, *length };

        return BodyFraming{ FramingKind::UntilClose, 0 };
    }

    // Whether the connection can carry another request after this
    // response (assuming the body was read to its end).
    inline bool connectionStaysOpen(const ResponseHead& head)
    {
        if (headerListContains(head.headers, "Connection", "close"))
            return false;

        if (head.minorVersion >= 1)
            return true;

        return headerListContains(head.headers, "Connection", "keep-alive");
    }

    /*
     * Chunked transfer coding
     */

    class ChunkedDecoder
    {
    public:
        enum class Status
        {
            NeedMore,
            Done,  // the terminating chunk and trailers have been consumed
            Error,
        };

        // Consumes bytes from `in`, appending decoded payload to `out`.
        // `consumed` is how many bytes of `in` were used; after Done it
        // stops at the end of the message, leaving any further bytes
        // untouched.
        Status feed(std::span<const std::byte> in, std::vector<std::byte>& out, std::size_t& consumed)
        {
            consumed = 0;

            if (m_state == State::Done)
                return Status::Done;

            if (m_state == State::Failed)
                return Status::Error;

            while (consumed < in.size())
            {
                const char c = static_cast<char>(in[consumed]);

                switch (m_state)
                {
                case State::SizeLine:
                    ++consumed;

                    if (c == '\n')
                    {
                        if (!finishSizeLine())
                            return Status::Error;
                    }
                    else
                    {
                        if (m_line.size() >= kMaxLineBytes)
                            return failWith("chunk size line too long");

                        m_line.push_back(c);
                    }
                    break;

                case State::Data:
                {
                    const std::uint64_t available = in.size() - consumed;
                    const std::size_t take = static_cast<std::size_t>(std::min(m_remaining, available));

                    out.insert(
                        out.end(),
                        in.begin() + static_cast<std::ptrdiff_t>(consumed),
                        in.begin() + static_cast<std::ptrdiff_t>(consumed + take));

                    consumed += take;
                    m_remaining -= take;

                    if (m_remaining == 0)
                    {
                        m_state = State::DataEnd;
                        m_sawCarriageReturn = false;
                    }
                    break;
                }

                case State::DataEnd:
                    ++consumed;

                    if (c == '\r' && !m_sawCarriageReturn)
                    {
                        m_sawCarriageReturn = true;
                        break;
                    }

                    if (c != '\n')
                        return failWith("missing line break after chunk data");

                    m_state = State::SizeLine;
                    m_line.clear();
                    break;

                case State::Trailer:
                    ++consumed;

                    if (c == '\n')
                    {
                        std::string_view line(m_line);

                        if (!line.empty() && line.back() == '\r')
                            line.remove_suffix(1);

                        if (line.empty())
                        {
                            m_state = State::Done;
                            return Status::Done;
                        }

                        m_line.clear(); // trailer fields are read and ignored
                    }
                    else
                    {
                        if (m_line.size() >= kMaxTrailerLineBytes)
                            return failWith("chunked trailer too long");

                        m_line.push_back(c);
                    }
                    break;

                case State::Done:
                case State::Failed:
                    return m_state == State::Done ? Status::Done : Status::Error;
                }
            }

            return Status::NeedMore;
        }

        const std::string& error() const noexcept
        {
            return m_error;
        }

    private:
        enum class State
        {
            SizeLine,
            Data,
            DataEnd,
            Trailer,
            Done,
            Failed,
        };

        static constexpr std::size_t kMaxLineBytes = 4096;
        static constexpr std::size_t kMaxTrailerLineBytes = 64 * 1024;

        Status failWith(std::string message)
        {
            m_error = std::move(message);
            m_state = State::Failed;
            return Status::Error;
        }

        bool finishSizeLine()
        {
            std::string_view line(m_line);

            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            // Ignore any chunk extension: "1a;name=value".
            const auto semicolon = line.find(';');

            if (semicolon != std::string_view::npos)
                line = line.substr(0, semicolon);

            line = trimWhitespace(line);

            const bool hexOnly = !line.empty() && std::all_of(line.begin(), line.end(), [](char ch)
            {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            });

            std::uint64_t size = 0;
            const auto result = std::from_chars(line.data(), line.data() + line.size(), size, 16);

            if (!hexOnly || result.ec != std::errc{} || result.ptr != line.data() + line.size())
            {
                failWith("invalid chunk size");
                return false;
            }

            m_line.clear();

            if (size == 0)
            {
                m_state = State::Trailer;
            }
            else
            {
                m_remaining = size;
                m_state = State::Data;
            }

            return true;
        }

        State m_state = State::SizeLine;
        std::string m_line;
        std::uint64_t m_remaining = 0;
        bool m_sawCarriageReturn = false;
        std::string m_error;
    };

    // The bytes that introduce a chunk of `size` payload bytes on the
    // wire: "<hex>\r\n". The payload and a trailing "\r\n" follow.
    inline std::string chunkHeader(std::size_t size)
    {
        char digits[2 * sizeof(std::size_t) + 1];
        const auto result = std::to_chars(digits, digits + sizeof(digits), size, 16);

        std::string header(digits, result.ptr);
        header += "\r\n";
        return header;
    }

    inline constexpr std::string_view kFinalChunk = "0\r\n\r\n";

    /*
     * Request head
     */

    // Percent-encodes the bytes of a request target that must never
    // appear raw on the request line (spaces, controls, non-ASCII, '"'),
    // leaving everything else -- including existing %XX escapes --
    // untouched. Prevents a stray space or line break in a URL from
    // splitting or corrupting the request.
    inline std::string encodeRequestTarget(std::string_view target)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";

        std::string result;
        result.reserve(target.size());

        for (const char ch : target)
        {
            const auto c = static_cast<unsigned char>(ch);

            if (c <= 0x20 || c >= 0x7F || c == '"')
            {
                result += '%';
                result += kHex[c >> 4];
                result += kHex[c & 0x0F];
            }
            else
            {
                result += ch;
            }
        }

        return result;
    }

    struct RequestHead
    {
        std::string_view method;
        std::string_view target;  // already encoded; see encodeRequestTarget()
        std::string_view host;    // value for the Host header

        // Headers supplied by the caller. Framing and connection
        // headers among them (Host, Content-Length, Transfer-Encoding,
        // Connection, ...) are IGNORED -- those are this layer's to
        // decide, and letting a caller set them is how request smuggling
        // starts.
        const dsbox_net::Metadata* headers = nullptr;

        std::optional<std::uint64_t> contentLength; // body of known size
        bool chunkedBody = false;                   // body of unknown size
        bool keepAlive = true;

        std::string_view userAgent; // sent if the caller supplied none; empty = none
    };

    inline bool isManagedRequestHeader(std::string_view name)
    {
        static constexpr std::string_view kManaged[] = {
            "Host", "Content-Length", "Transfer-Encoding", "Connection",
            "Keep-Alive", "Upgrade", "TE", "Trailer", "Expect", "Proxy-Connection",
        };

        return std::any_of(std::begin(kManaged), std::end(kManaged), [&](std::string_view managed)
        {
            return equalsIgnoreCase(name, managed);
        });
    }

    // Builds the request line and headers, ending with the blank line.
    // Throws NetException(UnsupportedOperation) for a method, target or
    // header that cannot be put on the wire safely.
    inline std::string serializeRequestHead(const RequestHead& request)
    {
        const auto reject = [](const std::string& why) -> void
        {
            throw dsbox_net::NetException(dsbox_net::NetError{
                dsbox_net::NetErrorCode::UnsupportedOperation, why, 0 });
        };

        if (!isToken(request.method))
            reject("invalid HTTP method");

        if (request.target.empty()
            || std::any_of(request.target.begin(), request.target.end(), [](char c)
            {
                const auto u = static_cast<unsigned char>(c);
                return u <= 0x20 || u == 0x7F;
            }))
        {
            reject("invalid request target");
        }

        std::string head;
        head.reserve(256);

        head += request.method;
        head += ' ';
        head += request.target;
        head += " HTTP/1.1\r\nHost: ";
        head += request.host;
        head += "\r\n";

        bool hasUserAgent = false;
        bool hasAccept = false;

        if (request.headers)
        {
            for (const auto& [name, value] : request.headers->entries())
            {
                if (!isToken(name))
                    reject("invalid header name: " + name);

                const bool badValue = std::any_of(value.begin(), value.end(), [](char c)
                {
                    return c == '\r' || c == '\n' || c == '\0';
                });

                if (badValue)
                    reject("invalid character in header value: " + name);

                if (isManagedRequestHeader(name))
                    continue;

                if (equalsIgnoreCase(name, "User-Agent"))
                    hasUserAgent = true;
                else if (equalsIgnoreCase(name, "Accept"))
                    hasAccept = true;

                head += name;
                head += ": ";
                head += trimWhitespace(value);
                head += "\r\n";
            }
        }

        if (!hasUserAgent && !request.userAgent.empty())
        {
            head += "User-Agent: ";
            head += request.userAgent;
            head += "\r\n";
        }

        if (!hasAccept)
            head += "Accept: */*\r\n";

        if (request.chunkedBody)
        {
            head += "Transfer-Encoding: chunked\r\n";
        }
        else if (request.contentLength)
        {
            head += "Content-Length: ";
            head += std::to_string(*request.contentLength);
            head += "\r\n";
        }

        if (!request.keepAlive)
            head += "Connection: close\r\n";

        head += "\r\n";
        return head;
    }
}
