#include <dsbox_net_tls/trust_manager.hpp>

#include <algorithm>

namespace dsbox_net_tls
{
    std::uint64_t TrustManager::addProvider(std::shared_ptr<ITrustProvider> provider, const dsbox::IModule* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const auto id = m_nextId++;
        m_providers.push_back(Entry{ id, std::move(provider), owner });

        return id;
    }

    bool TrustManager::removeProvider(std::uint64_t id)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const auto it = std::find_if(
            m_providers.begin(),
            m_providers.end(),
            [id](const Entry& entry) { return entry.id == id; });

        if (it == m_providers.end())
            return false;

        m_providers.erase(it);
        return true;
    }

    std::size_t TrustManager::removeOwner(const dsbox::IModule* owner)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        const auto originalSize = m_providers.size();

        m_providers.erase(
            std::remove_if(
                m_providers.begin(),
                m_providers.end(),
                [owner](const Entry& entry) { return entry.owner == owner; }),
            m_providers.end());

        return originalSize - m_providers.size();
    }

    std::vector<Certificate> TrustManager::collectAll() const
    {
        std::vector<std::shared_ptr<ITrustProvider>> snapshot;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            snapshot.reserve(m_providers.size());
            for (const auto& entry : m_providers)
                snapshot.push_back(entry.provider);
        }

        std::vector<Certificate> result;

        for (const auto& provider : snapshot)
        {
            auto certs = provider->trustedCertificates();

            result.insert(
                result.end(),
                std::make_move_iterator(certs.begin()),
                std::make_move_iterator(certs.end()));
        }

        return result;
    }

    std::size_t TrustManager::providerCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_providers.size();
    }
}
