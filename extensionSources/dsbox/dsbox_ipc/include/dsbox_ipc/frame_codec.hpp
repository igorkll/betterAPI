#pragma once

#include <dsbox_ipc/api.hpp>
#include <dsbox_ipc/frame.hpp>

#include <span>
#include <string>
#include <vector>

namespace dsbox_ipc
{
    // Incrementally turns a raw byte stream into Frames, and Frames back
    // into raw bytes, for one specific wire format. A codec instance is
    // stateful -- feed() buffers the tail of a not-yet-complete frame
    // between calls -- and belongs to exactly one direction of exactly
    // one wire. Never share one instance between two different byte
    // sources, and never feed it from more than one thread at a time.
    class DSBOX_IPC_API IFrameCodec
    {
    public:
        virtual ~IFrameCodec() = default;

        // Feeds newly received bytes; returns every frame that became
        // complete as a result of this call (0, 1, or more -- a single
        // feed() can finish several small queued frames at once, or
        // none if the bytes only extend a still-incomplete frame).
        // Throws std::runtime_error on malformed input (bad header,
        // unknown kind token, truncated/oversized length, ...).
        virtual std::vector<Frame> feed(std::span<const std::byte> bytes) = 0;

        virtual std::vector<std::byte> encode(const Frame& frame) = 0;
    };

    // Wire format: "KIND ID TARGET LENGTH\n" followed by exactly LENGTH
    // raw payload bytes (no trailing separator after the payload). KIND
    // is one of REQ/RES/NTF/ERR; TARGET is "-" for Response/Error frames
    // (which carry no target). Binary-safe payload -- the length prefix
    // means embedded '\n' or NUL bytes inside payload are fine. Intended
    // for wires nobody else shares, such as a subprocess's stdout/stdin
    // pipes.
    class DSBOX_IPC_API LengthPrefixedFrameCodec final : public IFrameCodec
    {
    public:
        std::vector<Frame> feed(std::span<const std::byte> bytes) override;
        std::vector<std::byte> encode(const Frame& frame) override;

    private:
        std::vector<std::byte> m_buffer;
    };

    // Wire format: "KIND ID TARGET PAYLOAD\n", one frame per line, where
    // PAYLOAD runs to the end of the line. PAYLOAD must not itself
    // contain '\n' (typically single-line JSON or similar text-safe
    // encoding) -- encode() throws std::invalid_argument if it does.
    // Intended for a process's own std::cin/std::cout, which a human or
    // an unrelated line-oriented reader on the other end may also be
    // looking at.
    class DSBOX_IPC_API TextLineFrameCodec final : public IFrameCodec
    {
    public:
        std::vector<Frame> feed(std::span<const std::byte> bytes) override;
        std::vector<std::byte> encode(const Frame& frame) override;

    private:
        std::string m_buffer;
    };
}
