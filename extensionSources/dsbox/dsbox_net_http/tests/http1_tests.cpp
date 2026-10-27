#include "test_framework.hpp"

#include <dsbox_net_http/http1.hpp>

#include <string>
#include <vector>

using namespace dsbox_net_http_tests;
using namespace dsbox_net_http::http1;

namespace
{
    std::span<const std::byte> asBytes(std::string_view text)
    {
        return { reinterpret_cast<const std::byte*>(text.data()), text.size() };
    }

    std::string toText(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // Feeds `text` to a fresh parser in one go.
    ResponseHeadParser::Status parseAll(ResponseHeadParser& parser, std::string_view text, std::size_t* consumedOut = nullptr)
    {
        std::size_t consumed = 0;
        const auto status = parser.feed(asBytes(text), consumed);

        if (consumedOut)
            *consumedOut = consumed;

        return status;
    }

    /*
     * ResponseHeadParser
     */

    void testParsesSimpleResponseHead()
    {
        ResponseHeadParser parser;
        std::size_t consumed = 0;

        const std::string text =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "\r\n";

        DSBOX_NET_HTTP_CHECK(parseAll(parser, text, &consumed) == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(consumed, text.size());
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().status, 200);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().reason, "OK");
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().minorVersion, 1);
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*parser.head().headers.get("content-type")), "text/plain");
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*parser.head().headers.get("Content-Length")), "5");
    }

    void testStopsAtEndOfHeadAndReportsBodyStart()
    {
        ResponseHeadParser parser;
        std::size_t consumed = 0;

        const std::string head = "HTTP/1.1 200 OK\r\nA: b\r\n\r\n";
        const std::string text = head + "BODYBYTES";

        DSBOX_NET_HTTP_CHECK(parseAll(parser, text, &consumed) == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(consumed, head.size());
    }

    void testParsesAcrossManySmallFeeds()
    {
        ResponseHeadParser parser;

        const std::string text = "HTTP/1.1 404 Not Found\r\nX-One: 1\r\nX-Two: 2\r\n\r\nrest";
        std::size_t totalConsumed = 0;
        ResponseHeadParser::Status status = ResponseHeadParser::Status::NeedMore;

        for (std::size_t i = 0; i < text.size() && status == ResponseHeadParser::Status::NeedMore; ++i)
        {
            std::size_t consumed = 0;
            status = parser.feed(asBytes(std::string_view(text).substr(i, 1)), consumed);
            totalConsumed += consumed;
        }

        DSBOX_NET_HTTP_CHECK(status == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().status, 404);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().reason, "Not Found");
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*parser.head().headers.get("X-Two")), "2");
        DSBOX_NET_HTTP_CHECK_EQ(totalConsumed, text.size() - 4); // everything except "rest"
    }

    void testSplitInsideTheTerminator()
    {
        // The CRLFCRLF arrives split between feeds, at every position.
        const std::string text = "HTTP/1.1 200 OK\r\nA: b\r\n\r\n";

        for (std::size_t split = 1; split < text.size(); ++split)
        {
            ResponseHeadParser parser;
            std::size_t consumedFirst = 0;
            std::size_t consumedSecond = 0;

            const auto first = parser.feed(asBytes(std::string_view(text).substr(0, split)), consumedFirst);
            const auto second = first == ResponseHeadParser::Status::NeedMore
                ? parser.feed(asBytes(std::string_view(text).substr(split)), consumedSecond)
                : first;

            DSBOX_NET_HTTP_CHECK(second == ResponseHeadParser::Status::Complete);
            DSBOX_NET_HTTP_CHECK_EQ(consumedFirst + consumedSecond, text.size());
        }
    }

    void testAcceptsBareLineFeeds()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 200 OK\nA: b\n\n") == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*parser.head().headers.get("A")), "b");
    }

    void testStatusLineWithoutReasonPhrase()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 204\r\n\r\n") == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().status, 204);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().reason, "");
    }

    void testHttp10IsRecognised()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.0 200 OK\r\n\r\n") == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().minorVersion, 0);
    }

    void testRepeatedHeadersAreKeptInOrder()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser,
            "HTTP/1.1 200 OK\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\n") == ResponseHeadParser::Status::Complete);

        const auto cookies = parser.head().headers.getAll("set-cookie");
        DSBOX_NET_HTTP_CHECK_EQ(cookies.size(), static_cast<std::size_t>(2));
        DSBOX_NET_HTTP_CHECK_EQ(std::string(cookies[0]), "a=1");
        DSBOX_NET_HTTP_CHECK_EQ(std::string(cookies[1]), "b=2");
    }

    void testHeaderValueWhitespaceIsTrimmed()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 200 OK\r\nA:   spaced out \t \r\n\r\n") == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*parser.head().headers.get("A")), "spaced out");
    }

    void testFoldedHeaderIsJoinedWithSpace()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 200 OK\r\nA: one\r\n  two\r\n\r\n") == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(std::string(*parser.head().headers.get("A")), "one two");
    }

    void testRejectsMalformedStatusLines()
    {
        const char* bad[] = {
            "garbage\r\n\r\n",
            "HTTP/2 200 OK\r\n\r\n",
            "HTTP/1.1 20 OK\r\n\r\n",
            "HTTP/1.1 2x0 OK\r\n\r\n",
            "HTTP/1.1 099 Too Low\r\n\r\n",
            "HTTP/1.1 200OK\r\n\r\n",
            "HTTP/1.1\r\n\r\n",
        };

        for (const char* text : bad)
        {
            ResponseHeadParser parser;
            DSBOX_NET_HTTP_CHECK(parseAll(parser, text) == ResponseHeadParser::Status::Error);
        }
    }

    void testRejectsMalformedHeaders()
    {
        const char* bad[] = {
            "HTTP/1.1 200 OK\r\nNoColonHere\r\n\r\n",
            "HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n",
            "HTTP/1.1 200 OK\r\nName : x\r\n\r\n",      // whitespace before the colon
            "HTTP/1.1 200 OK\r\n: empty-name\r\n\r\n",
            "HTTP/1.1 200 OK\r\n continuation-first\r\n\r\n",
        };

        for (const char* text : bad)
        {
            ResponseHeadParser parser;
            DSBOX_NET_HTTP_CHECK(parseAll(parser, text) == ResponseHeadParser::Status::Error);
        }
    }

    void testRejectsOversizedHead()
    {
        HeadLimits limits;
        limits.maxHeadBytes = 64;

        ResponseHeadParser parser(limits);
        const std::string text = "HTTP/1.1 200 OK\r\nX: " + std::string(200, 'a') + "\r\n\r\n";

        DSBOX_NET_HTTP_CHECK(parseAll(parser, text) == ResponseHeadParser::Status::Error);
    }

    void testRejectsOversizedHeadThatNeverTerminates()
    {
        HeadLimits limits;
        limits.maxHeadBytes = 64;

        ResponseHeadParser parser(limits);

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 200 OK\r\nX: " + std::string(200, 'a')) == ResponseHeadParser::Status::Error);
    }

    void testRejectsTooManyHeaders()
    {
        HeadLimits limits;
        limits.maxHeaderCount = 3;

        ResponseHeadParser parser(limits);
        std::string text = "HTTP/1.1 200 OK\r\n";

        for (int i = 0; i < 5; ++i)
            text += "H" + std::to_string(i) + ": v\r\n";

        text += "\r\n";

        DSBOX_NET_HTTP_CHECK(parseAll(parser, text) == ResponseHeadParser::Status::Error);
    }

    void testResetAllowsReuse()
    {
        ResponseHeadParser parser;

        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 301 Moved\r\nLocation: /x\r\n\r\n") == ResponseHeadParser::Status::Complete);
        parser.reset();
        DSBOX_NET_HTTP_CHECK(parseAll(parser, "HTTP/1.1 200 OK\r\n\r\n") == ResponseHeadParser::Status::Complete);
        DSBOX_NET_HTTP_CHECK_EQ(parser.head().status, 200);
        DSBOX_NET_HTTP_CHECK(!parser.head().headers.has("Location"));
    }

    /*
     * Framing
     */

    ResponseHead headOf(std::string_view text)
    {
        ResponseHeadParser parser;
        parseAll(parser, text);
        return parser.head();
    }

    std::optional<BodyFraming> framingOf(std::string_view method, std::string_view text)
    {
        std::string error;
        return determineFraming(method, headOf(text), error);
    }

    void testFramingContentLength()
    {
        const auto framing = framingOf("GET", "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n");

        DSBOX_NET_HTTP_CHECK(framing.has_value());
        DSBOX_NET_HTTP_CHECK(framing->kind == FramingKind::ContentLength);
        DSBOX_NET_HTTP_CHECK_EQ(framing->length, static_cast<std::uint64_t>(42));
    }

    void testFramingChunked()
    {
        const auto framing = framingOf("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");

        DSBOX_NET_HTTP_CHECK(framing.has_value());
        DSBOX_NET_HTTP_CHECK(framing->kind == FramingKind::Chunked);
    }

    void testFramingChunkedWinsOverContentLength()
    {
        const auto framing = framingOf("GET",
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n");

        DSBOX_NET_HTTP_CHECK(framing.has_value());
        DSBOX_NET_HTTP_CHECK(framing->kind == FramingKind::Chunked);
    }

    void testFramingTransferEncodingWithoutFinalChunkedReadsUntilClose()
    {
        const auto framing = framingOf("GET", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n");

        DSBOX_NET_HTTP_CHECK(framing.has_value());
        DSBOX_NET_HTTP_CHECK(framing->kind == FramingKind::UntilClose);
    }

    void testFramingNoLengthMeansUntilClose()
    {
        const auto framing = framingOf("GET", "HTTP/1.1 200 OK\r\n\r\n");

        DSBOX_NET_HTTP_CHECK(framing.has_value());
        DSBOX_NET_HTTP_CHECK(framing->kind == FramingKind::UntilClose);
    }

    void testFramingBodylessResponses()
    {
        DSBOX_NET_HTTP_CHECK(framingOf("HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n")->kind == FramingKind::None);
        DSBOX_NET_HTTP_CHECK(framingOf("GET", "HTTP/1.1 204 No Content\r\nContent-Length: 99\r\n\r\n")->kind == FramingKind::None);
        DSBOX_NET_HTTP_CHECK(framingOf("GET", "HTTP/1.1 304 Not Modified\r\n\r\n")->kind == FramingKind::None);
        DSBOX_NET_HTTP_CHECK(framingOf("GET", "HTTP/1.1 100 Continue\r\n\r\n")->kind == FramingKind::None);
    }

    void testFramingDuplicateEqualContentLengthIsAccepted()
    {
        const auto framing = framingOf("GET", "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Length: 7\r\n\r\n");
        DSBOX_NET_HTTP_CHECK(framing.has_value());
        DSBOX_NET_HTTP_CHECK_EQ(framing->length, static_cast<std::uint64_t>(7));

        const auto listed = framingOf("GET", "HTTP/1.1 200 OK\r\nContent-Length: 7, 7\r\n\r\n");
        DSBOX_NET_HTTP_CHECK(listed.has_value());
    }

    void testFramingRejectsBadContentLength()
    {
        const char* bad[] = {
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: +1\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 1 2\r\n\r\n",
            "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        };

        for (const char* text : bad)
        {
            std::string error;
            const auto framing = determineFraming("GET", headOf(text), error);

            DSBOX_NET_HTTP_CHECK(!framing.has_value());
            DSBOX_NET_HTTP_CHECK(!error.empty());
        }
    }

    void testConnectionStaysOpenRules()
    {
        DSBOX_NET_HTTP_CHECK(connectionStaysOpen(headOf("HTTP/1.1 200 OK\r\n\r\n")));
        DSBOX_NET_HTTP_CHECK(!connectionStaysOpen(headOf("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n")));
        DSBOX_NET_HTTP_CHECK(!connectionStaysOpen(headOf("HTTP/1.0 200 OK\r\n\r\n")));
        DSBOX_NET_HTTP_CHECK(connectionStaysOpen(headOf("HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\n")));
        DSBOX_NET_HTTP_CHECK(!connectionStaysOpen(headOf("HTTP/1.1 200 OK\r\nConnection: keep-alive, close\r\n\r\n")));
        DSBOX_NET_HTTP_CHECK(!connectionStaysOpen(headOf("HTTP/1.1 200 OK\r\nConnection: CLOSE\r\n\r\n")));
    }

    /*
     * ChunkedDecoder
     */

    struct DecodeResult
    {
        ChunkedDecoder::Status status = ChunkedDecoder::Status::NeedMore;
        std::string body;
        std::size_t consumed = 0;
    };

    DecodeResult decodeInPieces(std::string_view wire, std::size_t pieceSize)
    {
        ChunkedDecoder decoder;
        DecodeResult result;
        std::vector<std::byte> out;

        std::size_t position = 0;

        while (position < wire.size() && result.status == ChunkedDecoder::Status::NeedMore)
        {
            const auto piece = wire.substr(position, pieceSize);
            std::size_t consumed = 0;

            result.status = decoder.feed(asBytes(piece), out, consumed);
            position += consumed;
            result.consumed = position;
        }

        result.body = toText(out);
        return result;
    }

    void testChunkedDecodesSimpleBody()
    {
        const auto result = decodeInPieces("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, "hello world");
    }

    void testChunkedDecodesByteByByte()
    {
        const std::string wire = "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
        const auto result = decodeInPieces(wire, 1);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, "hello world");
        DSBOX_NET_HTTP_CHECK_EQ(result.consumed, wire.size());
    }

    void testChunkedHexSizesAndExtensions()
    {
        const auto result = decodeInPieces("A;name=value\r\n0123456789\r\n1f\r\n" + std::string(31, 'x') + "\r\n0\r\n\r\n", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, "0123456789" + std::string(31, 'x'));
    }

    void testChunkedTrailersAreSkipped()
    {
        const auto result = decodeInPieces("3\r\nabc\r\n0\r\nX-Checksum: 123\r\nX-Other: y\r\n\r\n", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, "abc");
    }

    void testChunkedStopsExactlyAtEndOfMessage()
    {
        const std::string message = "3\r\nabc\r\n0\r\n\r\n";
        const auto result = decodeInPieces(message + "NEXTRESPONSE", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.consumed, message.size());
    }

    void testChunkedAcceptsBareLineFeeds()
    {
        const auto result = decodeInPieces("3\nabc\n0\n\n", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, "abc");
    }

    void testChunkedBinaryPayloadWithEmbeddedLineBreaks()
    {
        // The payload itself contains "\r\n0\r\n\r\n" -- it must be
        // treated as data because the chunk size says so.
        const std::string payload = "ab\r\n0\r\n\r\ncd";
        const auto result = decodeInPieces("b\r\n" + payload + "\r\n0\r\n\r\n", 3);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, payload);
    }

    void testChunkedIncompleteNeedsMore()
    {
        const auto result = decodeInPieces("5\r\nhel", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::NeedMore);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, "hel");
    }

    void testChunkedRejectsBadSizes()
    {
        const char* bad[] = {
            "xyz\r\nabc\r\n0\r\n\r\n",
            "\r\n",
            "-5\r\nhello\r\n0\r\n\r\n",
            "+5\r\nhello\r\n0\r\n\r\n",
            "5 5\r\nhello\r\n0\r\n\r\n",
            "FFFFFFFFFFFFFFFFFF\r\nx\r\n",
        };

        for (const char* wire : bad)
        {
            const auto result = decodeInPieces(wire, 1000);
            DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Error);
        }
    }

    void testChunkedRejectsMissingLineBreakAfterData()
    {
        const auto result = decodeInPieces("3\r\nabcXX0\r\n\r\n", 1000);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Error);
    }

    void testChunkedStaysFailedAfterError()
    {
        ChunkedDecoder decoder;
        std::vector<std::byte> out;
        std::size_t consumed = 0;

        DSBOX_NET_HTTP_CHECK(decoder.feed(asBytes("zz\r\n"), out, consumed) == ChunkedDecoder::Status::Error);
        DSBOX_NET_HTTP_CHECK(decoder.feed(asBytes("0\r\n\r\n"), out, consumed) == ChunkedDecoder::Status::Error);
        DSBOX_NET_HTTP_CHECK(!decoder.error().empty());
    }

    void testChunkHeaderEncoding()
    {
        DSBOX_NET_HTTP_CHECK_EQ(chunkHeader(1), "1\r\n");
        DSBOX_NET_HTTP_CHECK_EQ(chunkHeader(255), "ff\r\n");
        DSBOX_NET_HTTP_CHECK_EQ(chunkHeader(4096), "1000\r\n");
    }

    void testChunkEncodeDecodeRoundTrip()
    {
        const std::string payload(10000, 'q');
        std::string wire;

        for (std::size_t offset = 0; offset < payload.size(); offset += 3000)
        {
            const std::string piece = payload.substr(offset, 3000);
            wire += chunkHeader(piece.size()) + piece + "\r\n";
        }

        wire += std::string(kFinalChunk);

        const auto result = decodeInPieces(wire, 777);

        DSBOX_NET_HTTP_CHECK(result.status == ChunkedDecoder::Status::Done);
        DSBOX_NET_HTTP_CHECK_EQ(result.body, payload);
    }

    /*
     * Request serialisation
     */

    void testSerializesMinimalRequest()
    {
        RequestHead request;
        request.method = "GET";
        request.target = "/index.html?x=1";
        request.host = "example.com";

        const std::string head = serializeRequestHead(request);

        DSBOX_NET_HTTP_CHECK_EQ(head,
            "GET /index.html?x=1 HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Accept: */*\r\n"
            "\r\n");
    }

    void testSerializesUserAgentAndCallerHeaders()
    {
        dsbox_net::Metadata headers;
        headers.add("Accept-Language", "en");
        headers.add("X-Custom", "  padded  ");

        RequestHead request;
        request.method = "GET";
        request.target = "/";
        request.host = "example.com";
        request.headers = &headers;
        request.userAgent = "DSBox/1.0";

        const std::string head = serializeRequestHead(request);

        DSBOX_NET_HTTP_CHECK(head.find("Accept-Language: en\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("X-Custom: padded\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("User-Agent: DSBox/1.0\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("Accept: */*\r\n") != std::string::npos);
    }

    void testCallerUserAgentAndAcceptWin()
    {
        dsbox_net::Metadata headers;
        headers.add("user-agent", "Mine/2");
        headers.add("ACCEPT", "text/html");

        RequestHead request;
        request.method = "GET";
        request.target = "/";
        request.host = "example.com";
        request.headers = &headers;
        request.userAgent = "DSBox/1.0";

        const std::string head = serializeRequestHead(request);

        DSBOX_NET_HTTP_CHECK(head.find("DSBox/1.0") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("*/*") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("user-agent: Mine/2\r\n") != std::string::npos);
    }

    void testFramingHeadersFromCallerAreIgnored()
    {
        dsbox_net::Metadata headers;
        headers.add("Host", "evil.example");
        headers.add("Content-Length", "999");
        headers.add("Transfer-Encoding", "chunked");
        headers.add("Connection", "upgrade");
        headers.add("Upgrade", "websocket");

        RequestHead request;
        request.method = "POST";
        request.target = "/";
        request.host = "example.com";
        request.headers = &headers;
        request.contentLength = 5;

        const std::string head = serializeRequestHead(request);

        DSBOX_NET_HTTP_CHECK(head.find("evil.example") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("999") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("websocket") == std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("Content-Length: 5\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("Transfer-Encoding") == std::string::npos);
    }

    void testSerializesChunkedBodyAndConnectionClose()
    {
        RequestHead request;
        request.method = "POST";
        request.target = "/upload";
        request.host = "example.com";
        request.chunkedBody = true;
        request.keepAlive = false;

        const std::string head = serializeRequestHead(request);

        DSBOX_NET_HTTP_CHECK(head.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("Connection: close\r\n") != std::string::npos);
        DSBOX_NET_HTTP_CHECK(head.find("Content-Length") == std::string::npos);
    }

    template<typename F>
    bool throwsNetError(F&& action)
    {
        try
        {
            action();
        }
        catch (const dsbox_net::NetException&)
        {
            return true;
        }

        return false;
    }

    void testRejectsHeaderInjection()
    {
        const auto serializeWithHeader = [](const std::string& name, const std::string& value)
        {
            dsbox_net::Metadata headers;
            headers.add(name, value);

            RequestHead request;
            request.method = "GET";
            request.target = "/";
            request.host = "example.com";
            request.headers = &headers;

            return serializeRequestHead(request);
        };

        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWithHeader("X-A", "v\r\nInjected: 1"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWithHeader("X-A", "v\ninjected"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWithHeader("X-A", std::string("a\0b", 3)); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWithHeader("Bad Name", "v"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWithHeader("X:A", "v"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWithHeader("", "v"); }));
    }

    void testRejectsBadMethodAndTarget()
    {
        const auto serializeWith = [](std::string_view method, std::string_view target)
        {
            RequestHead request;
            request.method = method;
            request.target = target;
            request.host = "example.com";
            return serializeRequestHead(request);
        };

        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWith("GET /x HTTP/1.1\r\nX:", "/"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWith("", "/"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWith("GET", "/a b"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWith("GET", "/a\r\nX: y"); }));
        DSBOX_NET_HTTP_CHECK(throwsNetError([&] { serializeWith("GET", ""); }));
    }

    void testEncodeRequestTarget()
    {
        DSBOX_NET_HTTP_CHECK_EQ(encodeRequestTarget("/plain?a=b&c=d"), "/plain?a=b&c=d");
        DSBOX_NET_HTTP_CHECK_EQ(encodeRequestTarget("/a b"), "/a%20b");
        DSBOX_NET_HTTP_CHECK_EQ(encodeRequestTarget("/a%20b"), "/a%20b"); // existing escapes are kept
        DSBOX_NET_HTTP_CHECK_EQ(encodeRequestTarget("/a\r\nX: y"), "/a%0D%0AX:%20y");
        DSBOX_NET_HTTP_CHECK_EQ(encodeRequestTarget("/\xC3\xA9"), "/%C3%A9");
        DSBOX_NET_HTTP_CHECK_EQ(encodeRequestTarget("/q\"x"), "/q%22x");
    }

    void testEncodedTargetIsAcceptedBySerializer()
    {
        RequestHead request;
        request.method = "GET";
        request.target = "/\xC3\xBCn\xC3\xAF code?q=a b";
        request.host = "example.com";

        const std::string encoded = encodeRequestTarget(request.target);
        request.target = encoded;

        DSBOX_NET_HTTP_CHECK(!throwsNetError([&] { serializeRequestHead(request); }));
    }
}

void runHttp1Tests()
{
    runTest("testParsesSimpleResponseHead", testParsesSimpleResponseHead);
    runTest("testStopsAtEndOfHeadAndReportsBodyStart", testStopsAtEndOfHeadAndReportsBodyStart);
    runTest("testParsesAcrossManySmallFeeds", testParsesAcrossManySmallFeeds);
    runTest("testSplitInsideTheTerminator", testSplitInsideTheTerminator);
    runTest("testAcceptsBareLineFeeds", testAcceptsBareLineFeeds);
    runTest("testStatusLineWithoutReasonPhrase", testStatusLineWithoutReasonPhrase);
    runTest("testHttp10IsRecognised", testHttp10IsRecognised);
    runTest("testRepeatedHeadersAreKeptInOrder", testRepeatedHeadersAreKeptInOrder);
    runTest("testHeaderValueWhitespaceIsTrimmed", testHeaderValueWhitespaceIsTrimmed);
    runTest("testFoldedHeaderIsJoinedWithSpace", testFoldedHeaderIsJoinedWithSpace);
    runTest("testRejectsMalformedStatusLines", testRejectsMalformedStatusLines);
    runTest("testRejectsMalformedHeaders", testRejectsMalformedHeaders);
    runTest("testRejectsOversizedHead", testRejectsOversizedHead);
    runTest("testRejectsOversizedHeadThatNeverTerminates", testRejectsOversizedHeadThatNeverTerminates);
    runTest("testRejectsTooManyHeaders", testRejectsTooManyHeaders);
    runTest("testResetAllowsReuse", testResetAllowsReuse);

    runTest("testFramingContentLength", testFramingContentLength);
    runTest("testFramingChunked", testFramingChunked);
    runTest("testFramingChunkedWinsOverContentLength", testFramingChunkedWinsOverContentLength);
    runTest("testFramingTransferEncodingWithoutFinalChunkedReadsUntilClose", testFramingTransferEncodingWithoutFinalChunkedReadsUntilClose);
    runTest("testFramingNoLengthMeansUntilClose", testFramingNoLengthMeansUntilClose);
    runTest("testFramingBodylessResponses", testFramingBodylessResponses);
    runTest("testFramingDuplicateEqualContentLengthIsAccepted", testFramingDuplicateEqualContentLengthIsAccepted);
    runTest("testFramingRejectsBadContentLength", testFramingRejectsBadContentLength);
    runTest("testConnectionStaysOpenRules", testConnectionStaysOpenRules);

    runTest("testChunkedDecodesSimpleBody", testChunkedDecodesSimpleBody);
    runTest("testChunkedDecodesByteByByte", testChunkedDecodesByteByByte);
    runTest("testChunkedHexSizesAndExtensions", testChunkedHexSizesAndExtensions);
    runTest("testChunkedTrailersAreSkipped", testChunkedTrailersAreSkipped);
    runTest("testChunkedStopsExactlyAtEndOfMessage", testChunkedStopsExactlyAtEndOfMessage);
    runTest("testChunkedAcceptsBareLineFeeds", testChunkedAcceptsBareLineFeeds);
    runTest("testChunkedBinaryPayloadWithEmbeddedLineBreaks", testChunkedBinaryPayloadWithEmbeddedLineBreaks);
    runTest("testChunkedIncompleteNeedsMore", testChunkedIncompleteNeedsMore);
    runTest("testChunkedRejectsBadSizes", testChunkedRejectsBadSizes);
    runTest("testChunkedRejectsMissingLineBreakAfterData", testChunkedRejectsMissingLineBreakAfterData);
    runTest("testChunkedStaysFailedAfterError", testChunkedStaysFailedAfterError);
    runTest("testChunkHeaderEncoding", testChunkHeaderEncoding);
    runTest("testChunkEncodeDecodeRoundTrip", testChunkEncodeDecodeRoundTrip);

    runTest("testSerializesMinimalRequest", testSerializesMinimalRequest);
    runTest("testSerializesUserAgentAndCallerHeaders", testSerializesUserAgentAndCallerHeaders);
    runTest("testCallerUserAgentAndAcceptWin", testCallerUserAgentAndAcceptWin);
    runTest("testFramingHeadersFromCallerAreIgnored", testFramingHeadersFromCallerAreIgnored);
    runTest("testSerializesChunkedBodyAndConnectionClose", testSerializesChunkedBodyAndConnectionClose);
    runTest("testRejectsHeaderInjection", testRejectsHeaderInjection);
    runTest("testRejectsBadMethodAndTarget", testRejectsBadMethodAndTarget);
    runTest("testEncodeRequestTarget", testEncodeRequestTarget);
    runTest("testEncodedTargetIsAcceptedBySerializer", testEncodedTargetIsAcceptedBySerializer);
}
