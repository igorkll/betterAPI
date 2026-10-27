#pragma once

#include <dsbox_net_socket/api.hpp>
#include <dsbox_net_socket/connection.hpp>

#include <dsbox/module.hpp>
#include <dsbox/owner.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace dsbox_net_socket
{
    // Maps a URL scheme to the IConnector that opens connections for it:
    // "http" -> plain TCP, "https" -> TCP + TLS, and so on.
    //
    // This is what decouples a protocol implementation (HTTP) from the
    // transport it runs over: the HTTP module asks the registry for the
    // "https" connector at request time and neither knows nor cares
    // whether it is OpenSSL, another TLS library, or a test double. A
    // TLS module registers its connector here when it starts.
    //
    // Thread-safe. Lookup is by scheme at call time, not captured at
    // registration, so modules may start in any order.
    class DSBOX_NET_SOCKET_API ConnectorRegistry
    {
    public:
        ConnectorRegistry();
        ~ConnectorRegistry();

        ConnectorRegistry(const ConnectorRegistry&) = delete;
        ConnectorRegistry& operator=(const ConnectorRegistry&) = delete;

        // Registers (or replaces) the connector for `scheme` (compared
        // case-insensitively). `owner` identifies the module that
        // registered it, for removeOwner() at shutdown -- same
        // convention as NetworkManager::registerHandler().
        void set(
            std::string scheme,
            std::shared_ptr<IConnector> connector,
            const dsbox::IModule* owner = dsbox::Anonymous);

        // The connector for `scheme`, or null if none is registered.
        std::shared_ptr<IConnector> get(std::string_view scheme) const;

        bool remove(std::string_view scheme);

        // Removes every connector registered with `owner`; returns how
        // many there were.
        std::size_t removeOwner(const dsbox::IModule* owner);

    private:
        struct Entry
        {
            std::shared_ptr<IConnector> connector;
            const dsbox::IModule* owner = nullptr;
        };

        mutable std::mutex m_mutex;
        std::map<std::string, Entry> m_entries;
    };
}
