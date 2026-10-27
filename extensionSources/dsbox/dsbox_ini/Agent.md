# dsbox_ini — Agent Notes

## Purpose
Small, dependency-free INI document parser/serializer. No filesystem
access — operates purely on `std::string_view` text in, `std::string`
text out. Used by `dsbox_config`.

## Key type
`IniDocument` (`ini_document.hpp/cpp`):
- `parse(text)` — throws `IniParseError` on a malformed section header,
  a line with no `=`, an empty key, an empty section name, or a
  **duplicate key within a section** / **duplicate section** (not
  silently overwritten — this is deliberate, catch config typos early).
- Sections preserve insertion order in `m_sections` (a
  `vector<pair<string, KeyValues>>`, not a map) so `serialize()` round-trips
  in the order they were written/declared. An implicit, unnamed default
  section (`""`) always exists at index 0 for top-level `key = value`
  lines that appear before any `[section]` header.
- `sections()` **excludes** the default section by design (it has no
  header to show the caller) — `keys()`/`hasKey()`/`get()`/`set()` all
  still work with `section == ""`. Any new code that enumerates "all
  sections" via `sections()` and then does something with each section's
  keys needs to remember the default section separately if it matters
  for that use case (see `dsbox_config`'s `patchOverridesText`, which
  had a bug here — now fixed and commented).
- `getBool()` accepts `true/1/yes/on` and `false/0/no/off` (case
  insensitive via `toLower`); anything else throws `IniParseError`.

## Testing
`tests/ini_document_tests.cpp` via the shared `test_framework.hpp`
runner. No changes made to this module this pass — it was audited
alongside `dsbox_config` (which depends on it) and found correct; the
bug found was in the *consumer*'s assumption about `sections()`, not in
`dsbox_ini` itself.
