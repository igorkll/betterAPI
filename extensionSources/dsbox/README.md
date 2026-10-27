# DSBox

DSBox is a general-purpose, modular C++20 framework. Its primary development
target is desktop GUI/browser-like applications (windowing, Vulkan
rendering, networking, a UI toolkit), but the architecture is deliberately
**environment-agnostic**: individual modules are usable standalone (e.g. in
a CLI tool, a headless service, or an Android ADB utility) and must not
take on hard dependencies on browser- or GUI-only concepts. Where a module
needs something environment-specific (a native window handle, text input,
etc.), it depends on a small interface (e.g. `dsbox::IWindowProvider`) owned
by `dsbox_core`, never on a concrete backend module.

## Layout

Every subproject follows the same shape:

```
dsbox_<name>/
  CMakeLists.txt
  include/dsbox_<name>/   public headers (dsbox_<name>.hpp umbrella + api.hpp export macros)
  src/                     implementation
  tests/                   unit tests (own CMakeLists.txt, test_framework.hpp, main.cpp)
```

Each subproject builds as its own shared library (DLL/.so) and is
independently documented in its own `Agent.md`.

### Core / foundation
- **dsbox_core** — module lifecycle (`ModuleManager`, `IModule`), the
  typed pub/sub + request/response bus (`CommunicationManager`), dynamic
  plugin loading, versioning, and the environment-abstracting interfaces
  (`IWindowProvider`, `ITextInputProvider`).
- **dsbox_ini** — small, dependency-free INI parser/serializer.
- **dsbox_config** — layered (defaults + overrides) persistent
  configuration on top of `dsbox_ini`, plus loading modules from an INI
  manifest.
- **dsbox_ipc** — framed, codec-based request/notify/response messaging
  over an arbitrary byte stream (pipes, sockets, etc.), built on
  `dsbox_scheduler` for timeouts.

### Platform / IO
dsbox_io, dsbox_process, dsbox_scheduler, dsbox_net (+ http/tls/socket/file
protocol handlers), dsbox_cli, dsbox_android_adb, dsbox_adb_tool.

### Presentation
dsbox_sdl (windowing), dsbox_vulkan (+ dsbox_vulkan_gui), dsbox_ui
(immediate/retained UI widgets), dsbox_ui_demo.

### Entry point
`launcher/` — loads modules per an INI manifest (see
`launcher/example_modules.ini`) and starts them via `ModuleManager`.

## Building

CMake + a C++20 compiler (MSVC, or GCC/Clang with `-std=c++20`).

```
cmake --preset <preset-from-CMakePresets.json>
cmake --build <build-dir>
ctest --test-dir <build-dir>
```

`DSBOX_BUILD_TESTS` (default ON) builds each subproject's `tests/` target.
`DSBOX_WITH_OPENSSL` (AUTO/ON/OFF) controls whether `dsbox_net_tls_openssl`
is built.

## Contributing

- Keep modules decoupled from any one GUI/browser backend; abstract
  environment-specific needs behind a core-owned interface.
- Preserve the public API of a module unless a fix requires a breaking
  change — call that out explicitly in the PR/commit.
- Every bug fix should come with a regression test in that module's
  `tests/` directory.
- See each subproject's `Agent.md` for module-specific conventions and
  known issues.
