#include <dsbox_net/url.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <deque>

namespace dsbox_net
{
    namespace
    {
        std::string toLower(std::string_view text)
        {
            std::string result(text);

            for (char& c : result)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            return result;
        }

        // True if `text` looks like "<scheme>:" at its very start, per
        // RFC 3986's scheme grammar (ALPHA *( ALPHA / DIGIT / "+" / "-" /
        // "." )). Used both by parse() (to find the scheme) and by
        // resolve() (to tell an absolute reference apart from a
        // relative one).
        bool startsWithScheme(std::string_view text, std::size_t& colonPos)
        {
            if (text.empty() || !std::isalpha(static_cast<unsigned char>(text[0])))
                return false;

            std::size_t i = 1;
            while (i < text.size())
            {
                const char c = text[i];

                if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.')
                {
                    ++i;
                    continue;
                }

                break;
            }

            if (i >= text.size() || text[i] != ':')
                return false;

            colonPos = i;
            return true;
        }

        // Splits `rest` (whatever follows the authority, or follows the
        // scheme colon when there is no authority) into path/query/fragment.
        void splitPathQueryFragment(
            std::string_view rest,
            std::string& outPath,
            std::string& outQuery,
            std::string& outFragment)
        {
            std::string_view pathPart = rest;

            const auto hashPos = pathPart.find('#');
            if (hashPos != std::string_view::npos)
            {
                outFragment = std::string(pathPart.substr(hashPos + 1));
                pathPart = pathPart.substr(0, hashPos);
            }

            const auto queryPos = pathPart.find('?');
            if (queryPos != std::string_view::npos)
            {
                outQuery = std::string(pathPart.substr(queryPos + 1));
                pathPart = pathPart.substr(0, queryPos);
            }

            outPath = std::string(pathPart);
        }

        // Resolves "." and ".." segments out of an absolute path, per
        // RFC 3986 section 5.2.4 -- only as much as merging a relative
        // reference's path onto a base path actually needs.
        std::string removeDotSegments(std::string_view path)
        {
            std::deque<std::string> output;

            std::size_t pos = 0;
            while (pos < path.size())
            {
                std::size_t next = path.find('/', pos + 1);
                if (next == std::string_view::npos)
                    next = path.size();

                std::string_view segment = path.substr(pos, next - pos); // includes leading '/'
                pos = next;

                // True once this segment is the last one the tokenizer
                // will produce (the `while` condition above won't run
                // again). Per the RFC 3986 5.2.4 pseudocode, a "."/".."
                // segment is always replaced by "/", not simply dropped
                // -- when it's the LAST segment, that replacement "/"
                // is the only thing left to move into the output, so it
                // must still be appended; otherwise it's immediately
                // consumed as the leading '/' of whatever real segment
                // follows, so appending nothing here is correct (that
                // next segment supplies the separator itself).
                const bool isLast = pos >= path.size();

                if (segment == "/." || segment == "/")
                {
                    // A bare "/" segment is NOT a dot-segment (e.g. the
                    // empty component between "//" in "/a//b", or an
                    // explicit trailing slash as in "/a/b/") -- it is
                    // always moved to the output verbatim, same as any
                    // other segment, never dropped just because the
                    // output already has something in it.
                    if (segment == "/")
                        output.push_back("/");
                    else if (isLast)
                        output.push_back("/");

                    continue;
                }

                if (segment == "/..")
                {
                    if (!output.empty())
                        output.pop_back();

                    if (isLast)
                        output.push_back("/");

                    continue;
                }

                output.push_back(std::string(segment));
            }

            if (output.empty())
                return "/";

            std::string result;
            for (const auto& segment : output)
                result += segment;

            return result;
        }

        // Parses the whole of `text` as a port number. An EMPTY string is
        // valid (RFC 3986 allows "host:") and yields no port; anything else
        // that is not entirely a number in 0..65535 -- including trailing
        // garbage like "80abc" -- is a failure.
        bool parsePort(std::string_view text, std::optional<std::uint16_t>& out)
        {
            out.reset();

            if (text.empty())
                return true;

            std::uint16_t port = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);

            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return false;

            out = port;
            return true;
        }

        std::uint16_t defaultPortFor(std::string_view scheme)
        {
            if (scheme == "http" || scheme == "ws")
                return 80;

            if (scheme == "https" || scheme == "wss")
                return 443;

            if (scheme == "ftp")
                return 21;

            if (scheme == "ftps")
                return 990;

            return 0;
        }
    }

    std::optional<Url> Url::parse(std::string_view text)
    {
        std::size_t colonPos = 0;

        if (!startsWithScheme(text, colonPos))
            return std::nullopt;

        Url url;
        url.scheme = toLower(text.substr(0, colonPos));

        std::string_view rest = text.substr(colonPos + 1);

        if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/')
        {
            rest = rest.substr(2);

            std::size_t authorityEnd = rest.find_first_of("/?#");
            if (authorityEnd == std::string_view::npos)
                authorityEnd = rest.size();

            std::string_view authority = rest.substr(0, authorityEnd);
            rest = rest.substr(authorityEnd);

            std::string_view hostPort = authority;

            const auto atPos = authority.find('@');
            if (atPos != std::string_view::npos)
            {
                url.userinfo = std::string(authority.substr(0, atPos));
                hostPort = authority.substr(atPos + 1);
            }

            if (!hostPort.empty() && hostPort.front() == '[')
            {
                // IPv6 literal: host runs up to the matching ']'.
                const auto closeBracket = hostPort.find(']');
                if (closeBracket == std::string_view::npos)
                    return std::nullopt;

                url.host = toLower(hostPort.substr(0, closeBracket + 1));

                std::string_view afterHost = hostPort.substr(closeBracket + 1);
                if (!afterHost.empty())
                {
                    if (afterHost.front() != ':')
                        return std::nullopt;

                    afterHost = afterHost.substr(1);

                    if (!parsePort(afterHost, url.port))
                        return std::nullopt;
                }
            }
            else
            {
                const auto colonInHost = hostPort.rfind(':');

                if (colonInHost != std::string_view::npos)
                {
                    url.host = toLower(hostPort.substr(0, colonInHost));

                    if (!parsePort(hostPort.substr(colonInHost + 1), url.port))
                        return std::nullopt;
                }
                else
                {
                    url.host = toLower(hostPort);
                }
            }

            // NOTE: an empty host here (e.g. "file:///etc/hosts", where
            // the authority between "//" and the next "/" is empty) is
            // NOT rejected -- RFC 3986 permits an empty authority, and
            // it is the standard way to write a local file:// URL (no
            // host = "this machine"). A scheme that actually requires a
            // host to be useful (http, ...) is expected to fail on its
            // own terms -- at connect time, with NetErrorCode::Dns or
            // similar -- rather than have Url::parse itself guess which
            // schemes need one.

            splitPathQueryFragment(rest, url.path, url.query, url.fragment);

            if (url.path.empty())
                url.path = "/";
        }
        else
        {
            // Authority-less scheme (e.g. "data:...", "mailto:...") --
            // everything after the colon is opaque as far as this class
            // is concerned, aside from a query/fragment split for
            // schemes that happen to use those delimiters too.
            splitPathQueryFragment(rest, url.path, url.query, url.fragment);
        }

        return url;
    }

    std::optional<Url> Url::resolve(std::string_view reference) const
    {
        std::size_t colonPos = 0;

        if (startsWithScheme(reference, colonPos))
            return Url::parse(reference);

        if (reference.size() >= 2 && reference[0] == '/' && reference[1] == '/')
        {
            std::string rebuilt = scheme + ":" + std::string(reference);
            return Url::parse(rebuilt);
        }

        Url result = *this;
        result.fragment.clear();

        std::string_view rest = reference;
        std::string newPath;
        std::string newQuery;
        std::string newFragment;
        splitPathQueryFragment(rest, newPath, newQuery, newFragment);

        const bool hasQuery = rest.substr(0, rest.find('#')).find('?') != std::string_view::npos;

        if (newPath.empty())
        {
            // Reference is only a query and/or fragment -- path is
            // unchanged, and (RFC 3986 5.2.2) so is the base query unless
            // the reference supplies its own.
            if (hasQuery)
                result.query = newQuery;
        }
        else if (newPath.front() == '/')
        {
            result.path = removeDotSegments(newPath);
            result.query = newQuery;
        }
        else
        {
            // Merge relative to the base path's directory (everything
            // up to and including its last '/').
            std::string basePath = path;
            const auto lastSlash = basePath.rfind('/');

            std::string merged = (lastSlash == std::string::npos)
                ? "/" + newPath
                : basePath.substr(0, lastSlash + 1) + newPath;

            result.path = removeDotSegments(merged);
            result.query = newQuery;
        }

        result.fragment = newFragment;

        return result;
    }

    std::string Url::toString() const
    {
        std::string result = scheme + ":";

        if (!host.empty())
        {
            result += "//";

            if (!userinfo.empty())
                result += userinfo + "@";

            result += host;

            if (port.has_value())
                result += ":" + std::to_string(*port);
        }

        result += path;

        if (!query.empty())
            result += "?" + query;

        if (!fragment.empty())
            result += "#" + fragment;

        return result;
    }

    std::uint16_t Url::effectivePort() const noexcept
    {
        if (port.has_value())
            return *port;

        return defaultPortFor(scheme);
    }

    bool Url::operator==(const Url& other) const noexcept
    {
        return scheme == other.scheme
            && userinfo == other.userinfo
            && host == other.host
            && port == other.port
            && path == other.path
            && query == other.query
            && fragment == other.fragment;
    }
}
