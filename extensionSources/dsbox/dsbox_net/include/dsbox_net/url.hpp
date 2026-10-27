#pragma once

#include <dsbox_net/api.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace dsbox_net
{
    // A parsed URL, split into the pieces NetworkManager and protocol
    // handlers actually need to make routing/connection decisions.
    // Deliberately NOT a general-purpose URI/IRI implementation (no
    // percent-decoding policy choices, no punycode, no relative-reference
    // edge cases beyond what resolve() below needs for HTTP redirects) --
    // scope creep there belongs in a dedicated URL library if this
    // project ever needs one; this is only as much parsing as routing by
    // scheme and connecting to host:port requires.
    //
    // `scheme` and `host` are normalized to lowercase (scheme and host
    // are case-insensitive per RFC 3986); everything else is kept exactly
    // as parsed. `path` always starts with '/' once normalized -- see
    // parse()'s own notes -- except for schemes with no authority
    // component at all (e.g. "data:"), where `host` is empty and `path`
    // is everything after the colon.
    struct DSBOX_NET_API Url
    {
        std::string scheme;
        std::string userinfo; // "user:pass" portion, without the trailing '@'; empty if absent
        std::string host;
        std::optional<std::uint16_t> port; // absent means "use the scheme's default"
        std::string path; // includes the leading '/'; empty only for authority-less schemes
        std::string query; // without the leading '?'
        std::string fragment; // without the leading '#'

        // Parses `text` as an absolute URL. Returns std::nullopt if it is
        // not well-formed enough to route: no scheme, a malformed
        // authority (e.g. an unterminated "[" IPv6 literal, or a port
        // that isn't a number), or similar syntax errors. An EMPTY host
        // (e.g. "file:///etc/hosts") is deliberately NOT one of these
        // failure cases -- see url.cpp's own note at the point that
        // matters for why.
        static std::optional<Url> parse(std::string_view text);

        // Resolves `reference` against *this as the base, per the
        // relative-reference resolution rules relevant to HTTP redirects
        // (RFC 3986 section 5, the common cases: absolute URL, protocol-
        // relative "//host/path", absolute-path "/path", and relative
        // path references with "." / ".." segments). Returns std::nullopt
        // if `reference` cannot be resolved into a usable absolute URL.
        std::optional<Url> resolve(std::string_view reference) const;

        // Reassembles the URL into its string form. Round-trips parse()
        // for anything parse() itself produced (normalization already
        // applied), but is not guaranteed to reproduce byte-for-byte
        // input that had e.g. unusual percent-encoding.
        std::string toString() const;

        // Effective port: `port` if set, otherwise the scheme's
        // well-known default (80 for http/ws, 443 for https/wss, 21 for
        // ftp, 990 for ftps); 0 if the scheme has no such convention.
        std::uint16_t effectivePort() const noexcept;

        bool operator==(const Url& other) const noexcept;
        bool operator!=(const Url& other) const noexcept
        {
            return !(*this == other);
        }
    };
}
