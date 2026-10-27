#include <dsbox_net_socket/connector_registry.hpp>

#include <cctype>
#include <iterator>
#include <vector>

namespace dsbox_net_socket
{
    namespace
    {
        std::string normalise(std::string_view scheme)
        {
            std::string result(scheme);

            for (char& c : result)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            return result;
        }
    }

    ConnectorRegistry::ConnectorRegistry() = default;
    ConnectorRegistry::~ConnectorRegistry() = default;

    void ConnectorRegistry::set(
        std::string scheme,
        std::shared_ptr<IConnector> connector,
        const dsbox::IModule* owner)
    {
        std::shared_ptr<IConnector> replaced;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto& entry = m_entries[normalise(scheme)];
            replaced = std::move(entry.connector); // destroyed outside the lock
            entry.connector = std::move(connector);
            entry.owner = owner;
        }
    }

    std::shared_ptr<IConnector> ConnectorRegistry::get(std::string_view scheme) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const auto it = m_entries.find(normalise(scheme));

        if (it == m_entries.end())
            return nullptr;

        return it->second.connector;
    }

    bool ConnectorRegistry::remove(std::string_view scheme)
    {
        std::shared_ptr<IConnector> removed;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            const auto it = m_entries.find(normalise(scheme));

            if (it == m_entries.end())
                return false;

            removed = std::move(it->second.connector);
            m_entries.erase(it);
        }

        return true;
    }

    std::size_t ConnectorRegistry::removeOwner(const dsbox::IModule* owner)
    {
        std::vector<std::shared_ptr<IConnector>> removed;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            for (auto it = m_entries.begin(); it != m_entries.end();)
            {
                if (it->second.owner == owner)
                {
                    removed.push_back(std::move(it->second.connector));
                    it = m_entries.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        return removed.size();
    }
}
