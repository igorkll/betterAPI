# dsbox_net_tls_systemca -- Agent Notes

## Purpose

`ITrustProvider` backed by the operating system's own trusted root
store. Windows reads the CryptoAPI "ROOT" store via `CertOpenStore`/
`CertEnumCertificatesInStore` (certificates returned DER-encoded).
Everywhere else (Linux, BSDs, macOS), there's no native-store API used
-- it falls back to `CaFileSources`: `SSL_CERT_FILE`/`SSL_CERT_DIR`
(same environment variables OpenSSL itself honours, checked first so
an administrator's override wins) then a hardcoded list of conventional
per-distro bundle file/directory paths (`defaultFileSources()`).

## Key behavior to preserve

- `readFileSources()`: the bundle-files loop returns as soon as the
  FIRST bundle file yields at least one certificate -- "the first
  usable bundle is THE bundle" (documented in `CaFileSources`'s own
  comments: bundles on one machine typically duplicate each other, and
  the subsequent `directories` scan is skipped entirely once a bundle
  succeeds). Don't "fix" this into merging every bundle found; it's
  intentional.
- Fail-closed: if nothing is readable, `trustedCertificates()` returns
  an empty list, never a "trust everything" fallback (see
  `SystemCaTrustProvider`'s own class-level doc comment).
- An explicit `CaFileSources` passed to the constructor makes the
  provider use exactly those files on EVERY platform (including
  Windows), bypassing the native store entirely -- the mechanism tests
  (and a deployment with its own CA policy) use instead of touching the
  real OS store.

## Flagged (not changed) -- see also dsbox_net_tls/Agent.md

Windows reads `CERT_SYSTEM_STORE_CURRENT_USER` only, never
`CERT_SYSTEM_STORE_LOCAL_MACHINE`. Documented, intentional, but a real
limitation worth the maintainer's attention: a machine-wide/
IT-installed root CA (common for corporate TLS-inspecting proxies or
internal CAs pushed via Group Policy) will not be trusted by this
provider alone. The header already recommends pairing with
`dsbox_net_tls_localca` for production use; whether to also read
LOCAL_MACHINE here is a design decision left to the maintainer since it
changes what gets trusted.

## No other bugs found

`systemca_trust_provider.cpp` and `systemca_module.cpp` read in full --
PEM-block splitting/dedup (`collectPemCertificates()`), file-size
sanity cap (`kMaxFileBytes`), and `is_regular_file` symlink-following
(needed since `/etc/ssl/certs/*` entries are typically hashed symlinks)
all check out.
