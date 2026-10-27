#include <dsbox_ipc/frame_codec.hpp>

#include <charconv>
#include <cstring>
#include <stdexcept>

namespace dsbox_ipc
{
    namespace
    {
        std::string_view kindToken(FrameKind kind)
        {
            switch (kind)
            {
            case FrameKind::Request:  return "REQ";
            case FrameKind::Response: return "RES";
            case FrameKind::Notify:   return "NTF";
            case FrameKind::Error:    return "ERR";
            }
            throw std::invalid_argument("dsbox_ipc: unknown FrameKind");
        }

        FrameKind parseKind(std::string_view token)
        {
            if (token == "REQ") return FrameKind::Request;
            if (token == "RES") return FrameKind::Response;
            if (token == "NTF") return FrameKind::Notify;
            if (token == "ERR") return FrameKind::Error;
            throw std::runtime_error("dsbox_ipc: malformed frame line, unknown kind token");
        }

        std::uint64_t parseId(std::string_view token)
        {
            std::uint64_t value{};
            const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
            if (result.ec != std::errc() || result.ptr != token.data() + token.size())
                throw std::runtime_error("dsbox_ipc: malformed frame line, bad id");
            return value;
        }
    }

    std::vector<std::byte> TextLineFrameCodec::encode(const Frame& frame)
    {
        const bool payloadHasNewline = std::memchr(frame.payload.data(), '\n', frame.payload.size()) != nullptr;
        if (payloadHasNewline)
            throw std::invalid_argument("dsbox_ipc: TextLineFrameCodec payload must not contain '\\n'");

        std::string line;
        line += kindToken(frame.kind);
        line += ' ';
        line += std::to_string(frame.id);
        line += ' ';
        line += frame.target.empty() ? "-" : frame.target;
        line += ' ';
        line.append(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
        line += '\n';

        std::vector<std::byte> encoded(line.size());
        std::memcpy(encoded.data(), line.data(), line.size());
        return encoded;
    }

    std::vector<Frame> TextLineFrameCodec::feed(std::span<const std::byte> bytes)
    {
        m_buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());

        std::vector<Frame> frames;
        size_t newlinePos;

        while ((newlinePos = m_buffer.find('\n')) != std::string::npos)
        {
            std::string_view line(m_buffer.data(), newlinePos);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);

            const size_t firstSpace = line.find(' ');
            const size_t secondSpace = line.find(' ', firstSpace == std::string_view::npos ? std::string_view::npos : firstSpace + 1);
            const size_t thirdSpace = line.find(' ', secondSpace == std::string_view::npos ? std::string_view::npos : secondSpace + 1);

            if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos || thirdSpace == std::string_view::npos)
                throw std::runtime_error("dsbox_ipc: malformed frame line, expected 'KIND ID TARGET PAYLOAD'");

            Frame frame;
            frame.kind = parseKind(line.substr(0, firstSpace));
            frame.id = parseId(line.substr(firstSpace + 1, secondSpace - firstSpace - 1));

            const std::string_view target = line.substr(secondSpace + 1, thirdSpace - secondSpace - 1);
            frame.target = (target == "-") ? std::string() : std::string(target);

            const std::string_view payload = line.substr(thirdSpace + 1);
            frame.payload.resize(payload.size());
            std::memcpy(frame.payload.data(), payload.data(), payload.size());

            frames.push_back(std::move(frame));
            m_buffer.erase(0, newlinePos + 1);
        }

        return frames;
    }
}
