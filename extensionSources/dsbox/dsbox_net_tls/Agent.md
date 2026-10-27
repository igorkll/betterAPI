# dsbox_net_tls -- Agent Notes

## Purpose

The TLS-library-agnostic half of the trust story: `ITrustProvider`
(one independent source of trusted root certificates -- OS store, a
local directory, a pinned set, ...) and `TrustManager` (aggregates
every registered provider into one flat, unordered,
`collectAll()`-able list of `Certificate`s). Deliberately knows nothing
about *why* a provider trusts what it trusts, and nothing about
TLS/OpenSSL -- that's `dsbox_net_tls_openssl` (or any other TLS-library
backend) built on top.

Concrete providers are separate modules: `dsbox_net_tls_localca` (a
bundled/locally-added CA directory) and `dsbox_net_tls_systemca` (the
OS's own trust store). Providers are purely additive, in no particular
order -- there is no "provider A overrides provider B".

## Thread safety

`TrustManager::collectAll()` snapshots the provider list under its own
mutex, then calls every provider's `trustedCertificates()` OUTSIDE that
lock -- so a slow or reentrant provider can never block
`addProvider()`/`removeProvider()`/`removeOwner()`, and a provider is
free to (un)register other providers from within its own
`trustedCertificates()` without deadlocking.

## Fail-closed by design

If no provider is registered (or every provider's source is
unreadable), `collectAll()` returns an empty list, `TlsConnector`
builds an empty `X509_STORE`, and every handshake then fails
verification -- this is the SAFE failure mode. `tls_connector.cpp`
specifically detects "zero trusted certificates" and adds a diagnostic
hint to the handshake error message ("is a trust provider module
loaded?") rather than silently falling back to trusting nothing *and*
reporting a confusing low-level OpenSSL error.

## No bugs found

`trust_manager.cpp`/`.hpp` and `tls_module.cpp` read in full; both
clean.

## Open design question for the maintainer (flagged, not changed)

`dsbox_net_tls_systemca`'s Windows path reads only the CURRENT_USER
CryptoAPI "ROOT" store (`CERT_SYSTEM_STORE_CURRENT_USER`), not
LOCAL_MACHINE. This is explicitly documented in
`systemca_trust_provider.hpp` as current, intentional behavior (with a
documented recommendation to pair it with `dsbox_net_tls_localca` for
production use) -- not a bug, but worth flagging: any CA installed
machine-wide (e.g. via Group Policy, MDM, or an administrator running
`certmgr.msc` in the "Local Computer" context -- a common pattern for
corporate TLS-inspecting proxies or internal CAs) is invisible to this
provider as it stands. Left unchanged since this is a trust-affecting
behavior change that deserves an explicit decision, not a silent fix.
