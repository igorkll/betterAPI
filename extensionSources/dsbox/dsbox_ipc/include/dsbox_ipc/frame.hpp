#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dsbox_ipc
{
    enum class FrameKind : std::uint8_t
    {
        Request,   // expects a Response or Error carrying the same id
        Response,  // successful reply to a Request
        Notify,    // fire-and-forget, no reply expected -- id is unused (0)
        Error,     // failed reply to a Request; payload is a UTF-8 message
    };

    // One addressed, correlated message. This is the unit IFrameCodec
    // parses raw bytes into and serializes back down to raw bytes --
    // IpcChannel operates purely in terms of Frame and never touches
    // bytes directly.
    struct Frame
    {
        FrameKind kind = FrameKind::Notify;
        std::uint64_t id = 0;
        std::string target;
        std::vector<std::byte> payload;
    };
}
