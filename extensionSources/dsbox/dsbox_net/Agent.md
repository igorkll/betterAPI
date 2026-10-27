# dsbox_net -- Agent Notes

## Purpose

Transport-agnostic HTTP(S)/file fetch core: `Url`, `Request`/`Response`,
`NetworkManager` (interceptor chain + redirect-following + scheme
routing), the pull-based body model (`IByteStream`/`IBodySource`/
`BodyPipe`), and cooperative cancellation (`CancellationSource`/Token`).
Nothing in this module knows how bytes actually move -- that's
`dsbox_net_socket`/`dsbox_net_http`/`dsbox_net_tls*`/`dsbox_net_file`,
each registering an `IProtocolHandler` for one or more URL schemes.

## Key types

- `Url` (`url.hpp/cpp`): scoped RFC 3986 parser/resolver. No
  percent-decoding, no punycode -- intentionally not general-purpose.
  IPv6 literals are stored in `host` WITH their brackets (e.g. `"[::1]"`),
  so callers that build a `Host:` header or a `toString()` by simple
  concatenation get correct output for free; don't strip them without
  checking every caller.
- `NetworkManager`: routes a `Request` through every registered
  `IInterceptor` (in order), then to the `IProtocolHandler` for
  `url.scheme`, and follows `Response::redirect` itself (so individual
  handlers never implement redirect policy). Redirect hops re-run the
  FULL interceptor chain (cookies/auth must be re-evaluated per-origin).
  `NetOptions::stripCredentialsOnCrossOriginRedirect` (default true)
  strips `Authorization`/`Proxy-Authorization`/`Cookie` on a
  cross-origin hop.
- Timeouts: `Request::timeout` bounds the WHOLE fetch including every
  redirect hop, not per-hop. Implemented by folding the caller's
  `CancellationToken` and a timer-driven one into a single internal
  `CancellationSource` (see `NetworkManager::fetch()`'s own comments) --
  no thread per in-flight timed fetch.
- Body model is PULL (`IByteStream::read()` blocks), not push/pause-
  resume -- deliberate simplification, documented as a scaling
  trade-off in `network_manager.hpp` (fine for a handful of concurrent
  transfers, would need revisiting for dozens of simultaneous in-flight
  resources).

## Bug fixed this pass

`removeDotSegments()` in `url.cpp` (RFC 3986 5.2.4) silently dropped the
trailing slash whenever the normalized path's LAST segment was a `.` or
`..` dot-segment (e.g. resolving `".."` against `/a/b/c` produced `/a`
instead of the correct `/a/`), and separately mis-handled bare `/`
segments (from an explicit trailing slash, or the empty component in a
`//` double-slash) by only preserving them when the output was still
empty, incorrectly collapsing e.g. `/a//b` to `/a/b`.

Fixed by tracking `isLast` per tokenized segment: a `.`/`..` segment
that is the last one processed still contributes a trailing `/` to the
output (per the RFC pseudocode, a dot-segment is always REPLACED by
`/`, not dropped -- dropping is only correct when the replacement `/`
is immediately consumed as the leading separator of a following real
segment), and a bare `/` segment is now always pushed to the output
verbatim regardless of whether the output is currently empty.

Regression tests: `testResolveTrailingDotDotSegmentKeepsTrailingSlash`,
`testResolveTrailingDotSegmentKeepsTrailingSlash`,
`testResolveTrailingSlashAfterDotDotSegmentsIsPreserved` in
`tests/url_tests.cpp`.

## Testing

`tests/url_tests.cpp`, `tests/body_tests.cpp`,
`tests/cancellation_tests.cpp`, `tests/network_manager_tests.cpp`. No
other bugs found in `network_manager.cpp` (redirect/timeout/cancel
interplay), `body.cpp` (backpressure pipe), or `net_module.cpp` (config
loading) during this pass -- all read in full and traced by hand.
