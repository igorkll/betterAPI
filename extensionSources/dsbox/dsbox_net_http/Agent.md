# dsbox_net_http -- Agent Notes

## Purpose

HTTP/1.1 client `IProtocolHandler` for `dsbox_net::NetworkManager`,
registered for both `http` and `https` (the latter works once a TLS
module -- e.g. `dsbox_net_tls_openssl` -- has registered an `https`
connector in the shared `dsbox_net_socket::ConnectorRegistry`; without
one, `https://` fails cleanly with `UnsupportedOperation` and `http://`
keeps working).

## Key types

- `http1.hpp` (header-only, by design -- shared between the handler and
  its own unit tests with no export macro needed): pure wire-format
  functions/state machines. `ResponseHeadParser` (incremental, tolerates
  bare-LF line endings per RFC 9112), `determineFraming()` (Content-
  Length / chunked / until-close, RFC 9112 6.3), `ChunkedDecoder`,
  `serializeRequestHead()` (rejects header/method/target injection --
  see `isManagedRequestHeader()` for which caller-supplied headers are
  silently dropped because this layer owns them).
- `HttpProtocolHandler::Impl`: `Transfer` (per-request state shared
  across the request thread and the body-streaming thread; `complete()`/
  `abort()` race safely -- whichever gets there first wins), `ThreadSet`
  (threads the handler owns; `shutdown()` aborts everything in flight
  and joins), `ConnectionPool` (keep-alive; LIFO reuse by `take()`
  popping from the back, LRU eviction by `put()` popping from the
  front).
- Redirect method-rewriting in `redirectFor()` follows browser-
  compatible rules: 303 -> GET (HEAD stays HEAD) regardless of original
  method; 301/302 of POST -> GET; everything else (307/308, or 301/302
  of a non-POST method) preserves method and body.

## Threading model

A request uses one thread while connecting/sending/waiting for the
head; once the head arrives, a SECOND thread streams the body into the
`Response`'s `BodyPipe`, so a `done` callback that reads the body
synchronously cannot deadlock. After the head arrives, the request's
timeout no longer applies (NetworkManager stops the timer once `done`
fires) -- but cancelling still aborts the body transfer via the
`Transfer`'s connection-close mechanism.

## No bugs found

This module was read in full (`http1.hpp`, `http_protocol_handler.cpp`,
`http_module.cpp`) and traced carefully for the usual HTTP-client
hazards: `Content-Length`/`Transfer-Encoding` smuggling-style ambiguity
(handled per RFC 9112 6.3 precedence), truncation on body size mismatch
(`job.reusable = false` whenever the server sends more/less than
promised), connection-pool race safety, and the retry-on-dead-pooled-
connection logic (`attempt == 0 && !sawResponseByte && replayable` --
only retried for idempotent methods, and only before any response byte
arrived). None of these turned up a correctness bug. Already has very
thorough existing test coverage in `tests/http1_tests.cpp` (including
`testSplitInsideTheTerminator`, which feeds the head-terminator byte by
byte at every split point -- this incidentally already exercises the
`ResponseHeadParser::findEndOfHead()` one-byte-conservative-lookahead
case and confirms it's a (harmless) buffering inefficiency, not a
correctness bug).

## Testing

`tests/http1_tests.cpp` (wire format), `tests/http_handler_tests.cpp`
(handler behavior against a loopback test server from
`dsbox_net_socket`).
