#pragma once

#include <dsbox_net_tls/api.hpp>
#include <dsbox_net_tls/certificate.hpp>

#include <vector>

namespace dsbox_net_tls
{
    // One independent source of trusted root certificates -- the OS
    // trust store, a directory of locally-added CAs, a hardcoded pinned
    // set for a specific deployment, or anything else. Each kind of
    // source is its own module/DLL (dsbox_net_tls_localca,
    // dsbox_net_tls_systemca, ...), registered independently with
    // TrustManager -- this interface is deliberately the only thing they
    // have in common; TrustManager itself has no idea which concrete
    // providers exist.
    class DSBOX_NET_TLS_API ITrustProvider
    {
    public:
        virtual ~ITrustProvider() = default;

        // Every certificate this provider currently considers trusted.
        // May perform real I/O (reading files, querying an OS store) --
        // called synchronously and potentially often (once per
        // TrustManager::collectAll() call, which itself will typically
        // run once per TLS handshake), so a provider backed by something
        // slow should cache internally if that matters for its use case;
        // this interface takes no position on caching or how fresh the
        // result needs to be.
        virtual std::vector<Certificate> trustedCertificates() const = 0;
    };
}
