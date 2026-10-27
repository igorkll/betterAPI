# dsbox_net_tls_openssl -- Agent Notes

## Purpose

`TlsConnector`: an `IConnector` that layers a TLS client handshake (via
OpenSSL) over whatever transport `IConnector` it's given (normally
`dsbox_net_socket::TcpConnector`), verifying the peer against a
`dsbox_net_tls::TrustManager`'s aggregated certificate list.
`OpenSslModule` registers it under the `"https"` scheme in the shared
`dsbox_net_socket::ConnectorRegistry`.

## Certificate verification -- read carefully before touching

This is the one place in the framework where a bug is a *security*
bug, not just a correctness one. Current behavior (verified correct by
hand-tracing against RFC 6125 / OpenSSL semantics):

- `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr)` with a NULL
  callback uses OpenSSL's default verify logic: a failed chain
  verification aborts the handshake (`SSL_connect()` returns <= 0), so
  trusting `result == 1` as "fully verified" in `handshake()` is
  correct -- don't add a redundant manual `SSL_get_verify_result()`
  check on the success path, it's already covered.
- Hostname matching is set up via `X509_VERIFY_PARAM` BEFORE
  `SSL_connect()`: `X509_VERIFY_PARAM_set1_ip_asc()` is tried first (it
  fails -- returns 0 -- for anything that isn't a literal IP), and only
  on that failure does it fall back to `SSL_set1_host()` (DNS name) +
  `SSL_set_tlsext_host_name()` (SNI). This is intentional and matches
  RFC 6066 (SNI must never carry an IP literal).
- `X509_CHECK_FLAG_NEVER_CHECK_SUBJECT` is set deliberately: NO
  fallback to the certificate's Subject CN, ever -- a name must appear
  in `subjectAltName`. This is stricter than some legacy browser
  behavior and should stay that way.
- `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS` rejects `f*.example.com`-style
  partial wildcards; only a leading `*.` label is accepted.
- `SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)` -- no max
  version pinned, so TLS 1.3 is used when available.
- Truncation-attack protection: a connection closed WITHOUT a TLS
  `close_notify` is reported as `NetErrorCode::ConnectionReset` (not a
  clean EOF) -- see `TransportContext::endOfStream` and
  `TlsConnection::failure()`. Don't change `read()` to treat
  `SSL_ERROR_SYSCALL`/EOF-without-close_notify as a clean end; that
  would reopen the classic HTTPS response-truncation attack.

## Timeout/cancellation race

`connect()`'s `Watchdog::disarm()` is documented to BLOCK until any
in-flight timeout callback has finished running before returning. This
is load-bearing: it's what lets `connect()` check `expired.load()`
*after* disarming and get a result that can't race with the watchdog
thread concurrently setting it. If you touch `Watchdog`, preserve that
"disarm waits for a running callback" guarantee or this logic breaks.

## Known minor gap (not fixed, low risk)

`SSL_CTX_set_alpn_protos()`'s return value is not checked in
`Impl::build()` (OpenSSL's own unusual convention: 0 = success,
nonzero = failure). A failure there would silently proceed without
ALPN negotiation rather than surfacing an error. Left as-is: it's an
availability/feature gap, not a trust/security issue, and `alpnProtocols`
is empty by default so most configurations never hit this path.

## No other bugs found

`tls_connector.cpp` and `openssl_module.cpp` read in full.
