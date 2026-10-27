#pragma once

#include <dsbox_net_tls/api.hpp>
#include <dsbox_net_tls/trust_provider.hpp>

#include <dsbox/module.hpp>
#include <dsbox/owner.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace dsbox_net_tls
{
    // Aggregates every ITrustProvider registered with it into one flat
    // list of trusted certificates (collectAll()) -- nothing more.
    // Deliberately knows nothing about *why* a provider trusts what it
    // trusts (system store vs. local files vs. anything else the project
    // adds later), and nothing about TLS/OpenSSL: turning the aggregated
    // result into something a TLS library can verify a peer's chain
    // against is a separate, later layer built on top of this one.
    //
    // Providers are independent and purely additive -- there is no
    // concept of one provider overriding or excluding another's
    // certificates, and no ordering guarantee on collectAll()'s result.
    // A provider that wants to be the ONLY source of trust achieves that
    // entirely on its own, by deciding not to register itself at all
    // (see e.g. dsbox_net_tls_systemca's own UseSystemCA option) -- this
    // class never makes that decision for anyone.
    //
    // Registered the same way as dsbox_net::NetworkManager's interceptor
    // list: an owner-tagged vector rather than a
    // dsbox::CommunicationManager, since providers have no name to key
    // on (there can be any number of them, not "at most one per name").
    //
    // Thread safety: every public method is safe to call concurrently
    // from multiple threads.
    class DSBOX_NET_TLS_API TrustManager
    {
    public:
        TrustManager() = default;

        TrustManager(const TrustManager&) = delete;
        TrustManager& operator=(const TrustManager&) = delete;

        // Registers `provider`. Order has no effect on collectAll()'s
        // result (see the class's own docs on providers being additive/
        // unordered). `owner`, if supplied, is what removeOwner() below
        // removes by -- pass `this` from inside the registering module.
        std::uint64_t addProvider(
            std::shared_ptr<ITrustProvider> provider,
            const dsbox::IModule* owner = dsbox::Anonymous);

        bool removeProvider(std::uint64_t id);

        // Removes every provider registered with this `owner`. Returns
        // how many were removed. The intended one-liner for a provider
        // module's onStop().
        std::size_t removeOwner(const dsbox::IModule* owner);

        // Every registered provider's trustedCertificates(), concatenated.
        // A snapshot of the provider list is taken first and each
        // provider is then called OUTSIDE any internal lock, so a slow
        // or reentrant provider can never block addProvider()/
        // removeProvider()/removeOwner(), and a provider is free to
        // (un)register other providers from within its own
        // trustedCertificates() without deadlocking.
        std::vector<Certificate> collectAll() const;

        std::size_t providerCount() const;

    private:
        struct Entry
        {
            std::uint64_t id = 0;
            std::shared_ptr<ITrustProvider> provider;
            const dsbox::IModule* owner = dsbox::Anonymous;
        };

        mutable std::mutex m_mutex;
        std::vector<Entry> m_providers;
        std::uint64_t m_nextId = 1;
    };
}
