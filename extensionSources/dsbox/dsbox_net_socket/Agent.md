# dsbox_net_socket -- Agent Notes

## Purpose

Protocol-agnostic transport layer: `IConnection` (blocking,
read/write/close, `close()` safe from any thread to abort a blocked
peer), `IConnector` (opens connections for host:port), and
`ConnectorRegistry` (maps a URL scheme to an `IConnector` -- the seam
between a protocol module like `dsbox_net_http` and whatever actually
opens the socket, including a TLS-wrapping connector layered on top of
the plain-TCP one).

## Key types

- `TcpConnector`: plain TCP over Winsock/BSD sockets, IPv4 + IPv6,
  tries every address `getaddrinfo()` returns in order. Non-blocking
  connect + `select`/`poll` in short (50ms) slices so a cancellation is
  noticed promptly even mid-connect. Each address gets its own capped
  `perAddressTimeout` slice (taken from the overall deadline) so one
  dead address can't eat the whole connect budget before the next
  address is even tried -- verified this falls through to the next
  address correctly on both `Attempt::Failed` and `Attempt::TimedOut`.
  `TcpConnection::close()` only `shutdown()`s the socket (wakes a
  blocked peer) -- the descriptor itself isn't released until the
  destructor, deliberately, because closing a descriptor another thread
  is blocked in `recv()` on is a race (the fd number can be reused).
- `ConnectorRegistry`: thread-safe scheme -> connector map, owner-
  tagged like `NetworkManager`'s interceptor list, for `removeOwner()`
  at module shutdown.

## No bugs found

Read `connection.hpp`, `connector_registry.hpp/cpp`,
`tcp_connector.hpp/cpp`, `socket_module.hpp/cpp` in full. Traced the
non-blocking-connect/poll state machine on both the Windows (`select`
with a `failed`/exceptfds set, since Windows signals a failed
non-blocking connect via exceptfds rather than writefds) and POSIX
(`poll` with `POLLOUT`) paths -- both fall back to an unconditional
`SO_ERROR` `getsockopt()` after waking, which is the portable-correct
way to distinguish success from failure regardless of which fd set or
revents flag actually fired, so neither path depends on exact
cross-platform `revents`/fd-set semantics. IPv6 literal handling
(`stripBrackets()` before `getaddrinfo()`) checked against
`dsbox_net::Url`'s own bracket-inclusive host storage -- consistent.

## Testing

`tests/tcp_connector_tests.cpp`, `tests/connector_registry_tests.cpp`,
`tests/loopback_server.hpp` (the in-process TCP server other modules'
tests, e.g. `dsbox_net_http`, build on).
