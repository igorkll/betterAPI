#include <dsbox_net/error.hpp>

namespace dsbox_net
{
    const char* toString(NetErrorCode code) noexcept
    {
        switch (code)
        {
            case NetErrorCode::InvalidUrl: return "InvalidUrl";
            case NetErrorCode::UnsupportedScheme: return "UnsupportedScheme";
            case NetErrorCode::UnsupportedOperation: return "UnsupportedOperation";
            case NetErrorCode::Dns: return "Dns";
            case NetErrorCode::Connect: return "Connect";
            case NetErrorCode::ConnectionReset: return "ConnectionReset";
            case NetErrorCode::Timeout: return "Timeout";
            case NetErrorCode::Tls: return "Tls";
            case NetErrorCode::Protocol: return "Protocol";
            case NetErrorCode::TooManyRedirects: return "TooManyRedirects";
            case NetErrorCode::Cancelled: return "Cancelled";
            case NetErrorCode::Internal: return "Internal";
        }

        return "Unknown";
    }
}
