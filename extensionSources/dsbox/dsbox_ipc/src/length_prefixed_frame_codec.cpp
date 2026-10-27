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
            throw std::runtime_error("dsbox_ipc: malformed frame header, unknown kind token");
        }

        std::string_view asStringView(std::span<const std::byte> bytes)
        {
            return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        template <typename T>
        T parseInteger(std::string_view token, const char* what)
        {
            T value{};
            const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
            if (result.ec != std::errc() || result.ptr != token.data() + token.size())
                throw std::runtime_error(std::string("dsbox_ipc: malformed frame header, bad ") + what);
            return value;
        }
    }

    std::vector<std::byte> LengthPrefixedFrameCodec::encode(const Frame& frame)
    {
        std::string header;
        header += kindToken(frame.kind);
        header += ' ';
        header += std::to_string(frame.id);
        header += ' ';
        header += frame.target.empty() ? "-" : frame.target;
        header += ' ';
        header += std::to_string(frame.payload.size());
        header += '\n';

        std::vector<std::byte> encoded(header.size() + frame.payload.size());
        std::memcpy(encoded.data(), header.data(), header.size());
        if (!frame.payload.empty())
            std::memcpy(encoded.data() + header.size(), frame.payload.data(), frame.payload.size());

        return encoded;
    }

    std::vector<Frame> LengthPrefixedFrameCodec::feed(std::span<const std::byte> bytes)
    {
        m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());

        std::vector<Frame> frames;
        size_t consumed = 0;

        for (;;)
        {
            const std::string_view remaining = asStringView(std::span<const std::byte>(m_buffer.data() + consumed, m_buffer.size() - consumed));
            const size_t newlinePos = remaining.find('\n');
            if (newlinePos == std::string_view::npos)
                break; // header not fully received yet

            const std::string_view headerLine = remaining.substr(0, newlinePos);

            const size_t firstSpace = headerLine.find(' ');
            const size_t secondSpace = headerLine.find(' ', firstSpace == std::string_view::npos ? std::string_view::npos : firstSpace + 1);
            const size_t thirdSpace = headerLine.find(' ', secondSpace == std::string_view::npos ? std::string_view::npos : secondSpace + 1);

            if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos || thirdSpace == std::string_view::npos)
                throw std::runtime_error("dsbox_ipc: malformed frame header, expected 'KIND ID TARGET LENGTH'");

            const FrameKind kind = parseKind(headerLine.substr(0, firstSpace));
            const std::uint64_t id = parseInteger<std::uint64_t>(headerLine.substr(firstSpace + 1, secondSpace - firstSpace - 1), "id");
            const std::string_view target = headerLine.substr(secondSpace + 1, thirdSpace - secondSpace - 1);
            const size_t length = parseInteger<size_t>(headerLine.substr(thirdSpace + 1), "length");

            const size_t headerBytes = newlinePos + 1;
            if (remaining.size() < headerBytes + length)
                break; // payload not fully received yet -- wait for more bytes

            Frame frame;
            frame.kind = kind;
            frame.id = id;
            frame.target = (target == "-") ? std::string() : std::string(target);
            frame.payload.assign(
                m_buffer.begin() + consumed + static_cast<std::ptrdiff_t>(headerBytes),
                m_buffer.begin() + consumed + static_cast<std::ptrdiff_t>(headerBytes + length));

            frames.push_back(std::move(frame));
            consumed += headerBytes + length;
        }

        if (consumed > 0)
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));

        return frames;
    }
}
