#include "test_framework.hpp"

#include <dsbox_net/url.hpp>

using namespace dsbox_net_tests;
using dsbox_net::Url;

namespace
{
    void testParseBasicHttp()
    {
        auto url = Url::parse("http://example.com/path?q=1#frag");
        DSBOX_NET_CHECK(url.has_value());

        DSBOX_NET_CHECK_EQ(url->scheme, "http");
        DSBOX_NET_CHECK_EQ(url->host, "example.com");
        DSBOX_NET_CHECK(!url->port.has_value());
        DSBOX_NET_CHECK_EQ(url->path, "/path");
        DSBOX_NET_CHECK_EQ(url->query, "q=1");
        DSBOX_NET_CHECK_EQ(url->fragment, "frag");
    }

    void testParseNormalizesSchemeAndHostCase()
    {
        auto url = Url::parse("HTTPS://Example.COM/Path");
        DSBOX_NET_CHECK(url.has_value());

        DSBOX_NET_CHECK_EQ(url->scheme, "https");
        DSBOX_NET_CHECK_EQ(url->host, "example.com");
        DSBOX_NET_CHECK_EQ(url->path, "/Path"); // path case is NOT normalized
    }

    void testParseExplicitPort()
    {
        auto url = Url::parse("http://example.com:8080/");
        DSBOX_NET_CHECK(url.has_value());
        DSBOX_NET_CHECK(url->port.has_value());
        DSBOX_NET_CHECK_EQ(*url->port, static_cast<std::uint16_t>(8080));
        DSBOX_NET_CHECK_EQ(url->effectivePort(), static_cast<std::uint16_t>(8080));
    }

    void testEffectivePortDefaults()
    {
        DSBOX_NET_CHECK_EQ(Url::parse("http://example.com/")->effectivePort(), static_cast<std::uint16_t>(80));
        DSBOX_NET_CHECK_EQ(Url::parse("https://example.com/")->effectivePort(), static_cast<std::uint16_t>(443));
        DSBOX_NET_CHECK_EQ(Url::parse("ftp://example.com/")->effectivePort(), static_cast<std::uint16_t>(21));
    }

    void testParseUserinfoAndNoPathBecomesSlash()
    {
        auto url = Url::parse("http://user:pass@example.com");
        DSBOX_NET_CHECK(url.has_value());
        DSBOX_NET_CHECK_EQ(url->userinfo, "user:pass");
        DSBOX_NET_CHECK_EQ(url->host, "example.com");
        DSBOX_NET_CHECK_EQ(url->path, "/");
    }

    void testParseIPv6Host()
    {
        auto url = Url::parse("http://[::1]:8080/path");
        DSBOX_NET_CHECK(url.has_value());
        DSBOX_NET_CHECK_EQ(url->host, "[::1]");
        DSBOX_NET_CHECK(url->port.has_value());
        DSBOX_NET_CHECK_EQ(*url->port, static_cast<std::uint16_t>(8080));
    }

    void testParseRejectsMissingScheme()
    {
        DSBOX_NET_CHECK(!Url::parse("example.com/path").has_value());
        DSBOX_NET_CHECK(!Url::parse("://example.com").has_value());
    }

    void testParseAllowsEmptyAuthorityForFileUrls()
    {
        // "file:///path" -- empty authority between the two slashes and
        // the next '/' -- is the standard way to write a local file://
        // URL and must parse, not be rejected. See Url::parse's own
        // docs for why an empty host is not treated as malformed.
        auto url = Url::parse("file:///etc/hosts");
        DSBOX_NET_CHECK(url.has_value());
        DSBOX_NET_CHECK_EQ(url->scheme, "file");
        DSBOX_NET_CHECK(url->host.empty());
        DSBOX_NET_CHECK_EQ(url->path, "/etc/hosts");
    }

    void testParseRejectsInvalidPort()
    {
        DSBOX_NET_CHECK(!Url::parse("http://example.com:abc/").has_value());
    }

    void testParseAuthorityLessScheme()
    {
        auto url = Url::parse("data:text/plain;base64,SGVsbG8=");
        DSBOX_NET_CHECK(url.has_value());
        DSBOX_NET_CHECK_EQ(url->scheme, "data");
        DSBOX_NET_CHECK(url->host.empty());
        DSBOX_NET_CHECK_EQ(url->path, "text/plain;base64,SGVsbG8=");
    }

    void testToStringRoundTrips()
    {
        auto url = Url::parse("https://example.com:8443/a/b?x=1#top");
        DSBOX_NET_CHECK(url.has_value());

        auto reparsed = Url::parse(url->toString());
        DSBOX_NET_CHECK(reparsed.has_value());
        DSBOX_NET_CHECK(*url == *reparsed);
    }

    void testResolveAbsolutePath()
    {
        auto base = Url::parse("https://example.com/a/b/c");
        auto resolved = base->resolve("/x/y");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->host, "example.com");
        DSBOX_NET_CHECK_EQ(resolved->path, "/x/y");
    }

    void testResolveRelativePath()
    {
        auto base = Url::parse("https://example.com/a/b/c");
        auto resolved = base->resolve("d/e");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/a/b/d/e");
    }

    void testResolveDotDotSegments()
    {
        auto base = Url::parse("https://example.com/a/b/c");
        auto resolved = base->resolve("../x");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/a/x");
    }

    void testResolveAbsoluteUrl()
    {
        auto base = Url::parse("https://example.com/a/b");
        auto resolved = base->resolve("http://other.example/z");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->scheme, "http");
        DSBOX_NET_CHECK_EQ(resolved->host, "other.example");
        DSBOX_NET_CHECK_EQ(resolved->path, "/z");
    }

    void testResolveProtocolRelative()
    {
        auto base = Url::parse("https://example.com/a/b");
        auto resolved = base->resolve("//other.example/z");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->scheme, "https"); // kept from base
        DSBOX_NET_CHECK_EQ(resolved->host, "other.example");
        DSBOX_NET_CHECK_EQ(resolved->path, "/z");
    }

    void testResolveQueryOnlyReference()
    {
        auto base = Url::parse("https://example.com/a/b?old=1");
        auto resolved = base->resolve("?new=2");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/a/b");
        DSBOX_NET_CHECK_EQ(resolved->query, "new=2");
    }

    void testResolveFragmentOnlyKeepsBaseQuery()
    {
        auto base = Url::parse("https://example.com/a/b?keep=1");
        auto resolved = base->resolve("#frag");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/a/b");
        DSBOX_NET_CHECK_EQ(resolved->query, "keep=1");
        DSBOX_NET_CHECK_EQ(resolved->fragment, "frag");
    }

    // Regression test: RFC 3986 5.2.4's remove_dot_segments algorithm
    // always replaces a trailing "."/".." segment with "/", never just
    // drops it -- a resolved path ending in a dot-segment must keep its
    // trailing slash (e.g. "/a/b/.." normalizes to "/a/", not "/a").
    // removeDotSegments() previously lost that trailing slash whenever
    // the LAST segment of the merged path was "." or "..".
    void testResolveTrailingDotDotSegmentKeepsTrailingSlash()
    {
        auto base = Url::parse("https://example.com/a/b/c");
        auto resolved = base->resolve("..");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/a/");
    }

    void testResolveTrailingDotSegmentKeepsTrailingSlash()
    {
        auto base = Url::parse("https://example.com/a/b/c");
        auto resolved = base->resolve(".");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/a/b/");
    }

    void testResolveTrailingSlashAfterDotDotSegmentsIsPreserved()
    {
        auto base = Url::parse("https://example.com/a/b/c");
        auto resolved = base->resolve("../../");
        DSBOX_NET_CHECK(resolved.has_value());
        DSBOX_NET_CHECK_EQ(resolved->path, "/");
    }

    void testParseRejectsPortWithTrailingGarbage()
    {
        DSBOX_NET_CHECK(!Url::parse("http://example.com:80abc/").has_value());
        DSBOX_NET_CHECK(!Url::parse("http://[::1]:80x/").has_value());
    }

    void testParseAllowsEmptyPort()
    {
        auto url = Url::parse("http://example.com:/x");
        DSBOX_NET_CHECK(url.has_value());
        DSBOX_NET_CHECK(!url->port.has_value());
        DSBOX_NET_CHECK_EQ(url->effectivePort(), static_cast<std::uint16_t>(80));
    }
}

void runUrlTests()
{
    runTest("testParseBasicHttp", testParseBasicHttp);
    runTest("testParseNormalizesSchemeAndHostCase", testParseNormalizesSchemeAndHostCase);
    runTest("testParseExplicitPort", testParseExplicitPort);
    runTest("testEffectivePortDefaults", testEffectivePortDefaults);
    runTest("testParseUserinfoAndNoPathBecomesSlash", testParseUserinfoAndNoPathBecomesSlash);
    runTest("testParseIPv6Host", testParseIPv6Host);
    runTest("testParseRejectsMissingScheme", testParseRejectsMissingScheme);
    runTest("testParseAllowsEmptyAuthorityForFileUrls", testParseAllowsEmptyAuthorityForFileUrls);
    runTest("testParseRejectsInvalidPort", testParseRejectsInvalidPort);
    runTest("testParseAuthorityLessScheme", testParseAuthorityLessScheme);
    runTest("testToStringRoundTrips", testToStringRoundTrips);
    runTest("testResolveAbsolutePath", testResolveAbsolutePath);
    runTest("testResolveRelativePath", testResolveRelativePath);
    runTest("testResolveDotDotSegments", testResolveDotDotSegments);
    runTest("testResolveAbsoluteUrl", testResolveAbsoluteUrl);
    runTest("testResolveProtocolRelative", testResolveProtocolRelative);
    runTest("testResolveQueryOnlyReference", testResolveQueryOnlyReference);
    runTest("testResolveFragmentOnlyKeepsBaseQuery", testResolveFragmentOnlyKeepsBaseQuery);
    runTest("testResolveTrailingDotDotSegmentKeepsTrailingSlash", testResolveTrailingDotDotSegmentKeepsTrailingSlash);
    runTest("testResolveTrailingDotSegmentKeepsTrailingSlash", testResolveTrailingDotSegmentKeepsTrailingSlash);
    runTest("testResolveTrailingSlashAfterDotDotSegmentsIsPreserved", testResolveTrailingSlashAfterDotDotSegmentsIsPreserved);
    runTest("testParseRejectsPortWithTrailingGarbage", testParseRejectsPortWithTrailingGarbage);
    runTest("testParseAllowsEmptyPort", testParseAllowsEmptyPort);
}
