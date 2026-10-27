# dsbox_ipc — Agent Notes

## Purpose
Framed request/notify/response messaging over an arbitrary byte stream
(pipe, socket, subprocess stdio, ...). Transport-agnostic: `IpcChannel`
is handed a `WriteFn` (bytes out) and fed bytes in via
`onBytesReceived()`; it doesn't own a socket/pipe itself. Depends on
`dsbox_scheduler` for request timeouts.

## Key types
- `Frame` (`frame.hpp`) — `FrameKind` (Request/Response/Notify/Error),
  `id`, `target`, `payload` (raw bytes).
- `IFrameCodec` (`frame_codec.hpp`) — stateful, incremental
  bytes→Frame(s) and Frame→bytes. One instance per direction per wire;
  never share across threads/streams. Two implementations:
  - `LengthPrefixedFrameCodec` — `"KIND ID TARGET LENGTH\n" + LENGTH raw
    bytes`. Binary-safe (embedded `\n`/NUL in payload is fine). Use for
    wires nobody else is reading as text (e.g. a subprocess pipe).
  - `TextLineFrameCodec` — `"KIND ID TARGET PAYLOAD\n"`, one frame per
    line; `encode()` throws `std::invalid_argument` if the payload
    contains `\n`. Use when a human or an unrelated line-oriented reader
    may also be looking at the stream.
- `IpcChannel` (`ipc_channel.hpp/cpp`) — ties a codec + write function
  together: `send()`/`notify()`, `request()`/`requestAsync()` (with
  per-request or channel-default timeout via a `Scheduler`, falling back
  to a lazily-created private single-thread scheduler if none is
  supplied or the shared one is stopped), `registerHandler()` for
  inbound Requests, `close()` (fails every still-pending request with
  `ok=false`).

## Known-subtle areas (read before touching)
- Pending-request bookkeeping (`detail::PendingTable`) is reference
  counted via `shared_ptr` and reached from timer callbacks only via
  `weak_ptr`, specifically so an in-flight timer never keeps a channel
  (or its callbacks) alive past `close()`/destruction.
- `handleFrame()`/`close()`/`requestAsync()` all release their mutex
  *before* invoking a `ResponseCallback` or destroying a retired
  timer/entry — callbacks are arbitrary user code and may legally call
  back into the same channel. Preserve this "release lock, then call
  out" ordering in any edit.
- `armTimeout()` falls back from the shared `Scheduler` to a private one
  if `scheduler->after()` returns an empty handle (shared scheduler
  stopped) — don't change this to treat an empty handle as "no timeout
  needed", it means the opposite.

## Testing
`tests/frame_codec_tests.cpp`, `tests/ipc_channel_tests.cpp`, via the
shared `test_framework.hpp` runner. No bugs found this pass in either
codec or `IpcChannel`; both were read in full.
