# dsbox_net_tls_localca -- Agent Notes

## Purpose

`ITrustProvider` backed by a local directory of CA certificate files
(`*.pem`/`*.der`/`*.crt`), split into a "default" directory (shipped
alongside the executable, under `config/default/trusted_ca/`) and an
"override" directory (`config/trusted_ca/`) -- both read on every
`trustedCertificates()` call, concatenated. Directory/file resolution
and reading go through `dsbox::Overridable` slots
(`m_resolveDir`/`m_readDir`), so tests (and a host with different path
conventions) can substitute canned data without touching the real
filesystem.

## Key behavior

- `parseCertificatesFromFileContent()`: a file is treated as PEM if it
  contains at least one `-----BEGIN CERTIFICATE-----` marker anywhere
  (`looksLikePem()`), in which case EVERY PEM block in it is split out
  as its own `Certificate` (a bundle file with several certs is fully
  supported); otherwise the whole file is treated as a single DER
  certificate. A truncated trailing PEM block (BEGIN with no matching
  END) is silently ignored rather than erroring.
- Missing default/override directories are NOT an error -- see
  `defaultReadCaDirectory()`'s early return on `!fs::exists(...)`. A
  host that ships no local CAs still works; it just contributes nothing
  from this provider.

## No bugs found

Read `localca_trust_provider.cpp` and `localca_module.cpp` in full.
