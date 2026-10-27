#include "test_framework.hpp"

#include <dsbox_ipc/frame_codec.hpp>

#include <cstring>
#include <stdexcept>
#include <string>

using namespace dsbox_ipc_tests;
using namespace dsbox_ipc;

namespace
{
    std::vector<std::byte> toBytes(const std::string& text)
    {
        std::vector<std::byte> bytes(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
        return bytes;
    }

    std::string toString(const std::vector<std::byte>& bytes)
    {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    void testLengthPrefixedRoundTrip()
    {
        LengthPrefixedFrameCodec encoder;
        LengthPrefixedFrameCodec decoder;

        Frame original;
        original.kind = FrameKind::Request;
        original.id = 42;
        original.target = "myservice.method";
        original.payload = toBytes("hello world");

        const std::vector<std::byte> wire = encoder.encode(original);
        const std::vector<Frame> decoded = decoder.feed(wire);

        DSBOX_IPC_CHECK_EQ(decoded.size(), 1u);
        DSBOX_IPC_CHECK(decoded[0].kind == FrameKind::Request);
        DSBOX_IPC_CHECK_EQ(decoded[0].id, 42u);
        DSBOX_IPC_CHECK_EQ(decoded[0].target, original.target);
        DSBOX_IPC_CHECK_EQ(toString(decoded[0].payload), "hello world");
    }

    void testLengthPrefixedBinaryPayloadWithEmbeddedNewline()
    {
        LengthPrefixedFrameCodec codec;

        Frame original;
        original.kind = FrameKind::Notify;
        original.target = "raw.bytes";
        original.payload = { std::byte{0x00}, std::byte{'\n'}, std::byte{0xFF}, std::byte{'\n'} };

        const std::vector<Frame> decoded = codec.feed(codec.encode(original));

        DSBOX_IPC_CHECK_EQ(decoded.size(), 1u);
        DSBOX_IPC_CHECK(decoded[0].payload == original.payload);
    }

    void testLengthPrefixedSplitAcrossFeedCalls()
    {
        LengthPrefixedFrameCodec encoder;
        LengthPrefixedFrameCodec decoder;

        Frame original;
        original.kind = FrameKind::Response;
        original.id = 7;
        original.payload = toBytes("payload-data");

        const std::vector<std::byte> wire = encoder.encode(original);

        // Feed one byte at a time to prove partial frames are buffered
        // correctly rather than assuming a full frame arrives per call.
        std::vector<Frame> decoded;
        for (const std::byte b : wire)
        {
            auto frames = decoder.feed(std::span<const std::byte>(&b, 1));
            for (auto& f : frames)
                decoded.push_back(std::move(f));
        }

        DSBOX_IPC_CHECK_EQ(decoded.size(), 1u);
        DSBOX_IPC_CHECK_EQ(decoded[0].id, 7u);
        DSBOX_IPC_CHECK_EQ(toString(decoded[0].payload), "payload-data");
    }

    void testLengthPrefixedMultipleFramesInOneFeed()
    {
        LengthPrefixedFrameCodec encoder;
        LengthPrefixedFrameCodec decoder;

        Frame a;
        a.kind = FrameKind::Notify;
        a.target = "a";
        a.payload = toBytes("one");

        Frame b;
        b.kind = FrameKind::Notify;
        b.target = "b";
        b.payload = toBytes("two");

        std::vector<std::byte> wire = encoder.encode(a);
        const std::vector<std::byte> wireB = encoder.encode(b);
        wire.insert(wire.end(), wireB.begin(), wireB.end());

        const std::vector<Frame> decoded = decoder.feed(wire);

        DSBOX_IPC_CHECK_EQ(decoded.size(), 2u);
        DSBOX_IPC_CHECK_EQ(decoded[0].target, "a");
        DSBOX_IPC_CHECK_EQ(decoded[1].target, "b");
    }

    void testLengthPrefixedThrowsOnMalformedHeader()
    {
        LengthPrefixedFrameCodec codec;
        bool threw = false;
        try
        {
            codec.feed(toBytes("NOT_A_KIND garbage\n"));
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        DSBOX_IPC_CHECK(threw);
    }

    void testTextLineRoundTrip()
    {
        TextLineFrameCodec encoder;
        TextLineFrameCodec decoder;

        Frame original;
        original.kind = FrameKind::Request;
        original.id = 1;
        original.target = "myservice.method";
        original.payload = toBytes("{\"jsonrpc\":true}");

        const std::vector<Frame> decoded = decoder.feed(encoder.encode(original));

        DSBOX_IPC_CHECK_EQ(decoded.size(), 1u);
        DSBOX_IPC_CHECK_EQ(decoded[0].target, original.target);
        DSBOX_IPC_CHECK_EQ(toString(decoded[0].payload), "{\"jsonrpc\":true}");
    }

    // Known limitation, deliberately documented rather than silently
    // "working": a target whose literal name is "-" is indistinguishable
    // on the wire from an empty target, so it round-trips as empty. Not a
    // problem in practice (targets are dotted names like
    // "myservice.method"), but real enough to pin down with a test.
    void testTextLineLiteralDashTargetCollidesWithEmptySentinel()
    {
        TextLineFrameCodec encoder;
        TextLineFrameCodec decoder;

        Frame original;
        original.kind = FrameKind::Notify;
        original.target = "-";
        original.payload = toBytes("x");

        const std::vector<Frame> decoded = decoder.feed(encoder.encode(original));

        DSBOX_IPC_CHECK_EQ(decoded.size(), 1u);
        DSBOX_IPC_CHECK_EQ(decoded[0].target, "");
    }

    void testTextLineEmptyTargetSentinel()
    {
        TextLineFrameCodec encoder;
        TextLineFrameCodec decoder;

        Frame original;
        original.kind = FrameKind::Response;
        original.id = 5;
        original.target = ""; // empty target -> encoded as literal "-"
        original.payload = toBytes("ok");

        const std::vector<Frame> decoded = decoder.feed(encoder.encode(original));

        DSBOX_IPC_CHECK_EQ(decoded.size(), 1u);
        DSBOX_IPC_CHECK_EQ(decoded[0].target, "");
    }

    void testTextLineThrowsOnEmbeddedNewline()
    {
        TextLineFrameCodec codec;

        Frame frame;
        frame.kind = FrameKind::Notify;
        frame.target = "x";
        frame.payload = toBytes("line one\nline two");

        bool threw = false;
        try
        {
            codec.encode(frame);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        DSBOX_IPC_CHECK(threw);
    }
}

void runFrameCodecTests()
{
    runTest("LengthPrefixedRoundTrip", testLengthPrefixedRoundTrip);
    runTest("LengthPrefixedBinaryPayloadWithEmbeddedNewline", testLengthPrefixedBinaryPayloadWithEmbeddedNewline);
    runTest("LengthPrefixedSplitAcrossFeedCalls", testLengthPrefixedSplitAcrossFeedCalls);
    runTest("LengthPrefixedMultipleFramesInOneFeed", testLengthPrefixedMultipleFramesInOneFeed);
    runTest("LengthPrefixedThrowsOnMalformedHeader", testLengthPrefixedThrowsOnMalformedHeader);
    runTest("TextLineRoundTrip", testTextLineRoundTrip);
    runTest("TextLineLiteralDashTargetCollidesWithEmptySentinel", testTextLineLiteralDashTargetCollidesWithEmptySentinel);
    runTest("TextLineEmptyTargetSentinel", testTextLineEmptyTargetSentinel);
    runTest("TextLineThrowsOnEmbeddedNewline", testTextLineThrowsOnEmbeddedNewline);
}
