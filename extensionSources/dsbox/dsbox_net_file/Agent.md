# dsbox_net_file -- Agent Notes

## Purpose

`IProtocolHandler` for the `file://` scheme: read-only local-filesystem
access through `NetworkManager`, routed the same way an HTTP fetch
would be. Only `Request::url` matters -- `method`, `body`, `query`, and
`fragment` are all ignored (there's nothing to "post" to a local file
in this module; writing to local files is explicitly out of scope,
left for a future, not-yet-designed part of the project).

## Key behavior

- `FilePathStyle` (`PosixOnly`/`WindowsOnly`/`Both`, resolved from
  `FilePathStyle::Default` + `[net.file].path_style` config in
  `FileModule::onInit()` -- never auto-detected at the URL-parsing
  level, deliberately: the doc comment calls out that auto-detecting
  "does this look like a Windows path" per-request is exactly the kind
  of ambiguity that turns into a path-traversal or wrong-file surprise
  on whichever platform didn't expect it.
- `nativePathFromUrlPath()`: percent-decodes the URL path (rejecting an
  embedded NUL from `%00`, which would silently truncate the path in a
  C-string-based filesystem API), then matches/converts a Windows drive
  path (`/C:/Users/x` -> `C:\Users\x`) per style.
- `file://host/path` with a non-empty, non-`localhost` host is
  explicitly rejected (`UnsupportedOperation`) rather than silently
  read as if it were local -- a remote-looking file:// URL would
  otherwise read the WRONG file on the local machine.
- The actual filesystem touch goes through a `dsbox::Overridable`
  (`"net.file.open"` by default) so tests/sandboxed hosts can fake or
  restrict disk access without touching `defaultFileOpen()`.
- `NotFound`/`Denied` become normally-delivered `Response`s (matching
  `StatusClass`), not `NetError`s -- consistent with how an HTTP
  handler turns a 404 into a `Response` rather than a delivery failure.
  Everything else (e.g. a directory where a file was expected) becomes
  a real `NetError`.

## No bugs found

`file_protocol_handler.cpp`, `file_module.cpp` read in full --
percent-decode boundary checks, Windows-drive-path detection/
conversion, and the `IfstreamByteStream`'s close()-from-another-thread
mutex handling all check out.
