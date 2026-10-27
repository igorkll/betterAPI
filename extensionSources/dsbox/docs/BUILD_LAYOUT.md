# DSBox build output & config layout

This describes two things that used to be flat and are not anymore:
where built DLL/.so/.exe files land, and where configuration lives.
Referenced from comments in the top-level `CMakeLists.txt`,
`cmake/DSBoxOutputDirs.cmake`, `cmake/DSBoxDelayLoad.cmake`,
`dsbox_core/include/dsbox/bootstrap_paths.hpp`, and the `main.cpp` /
`launch_options.*` / `gui_mode.cpp` files of `dsbox_ui_demo`,
`dsbox_adb_tool` and `launcher`.

## 1. Build output: one `fin-out/` root, with `bin/` subfolders inside it

Previously every target's DLL/.so/.exe was routed into one flat `bin/`.
Now every build output category -- `bin/`, `lib/`, `config/`,
`shaders/` -- lives under one top-level output root,
`${CMAKE_BINARY_DIR}/fin-out/` (the `DSBOX_OUTPUT_ROOT` CMake variable,
set once in the top-level `CMakeLists.txt`), rather than directly
under the CMake build directory:

```
<build>/fin-out/
  DSBoxModLoader.exe  standalone game launcher
  bin/
    framework/
      core/        DSBoxCore, DSBoxIni, DSBoxConfig, DSBoxProcess
      scheduler/   DSBoxScheduler, DSBoxSchedulerModule
      ipc/         DSBoxIpc
      io/          DSBoxIO
      android/     DSBoxAndroidAdb
      cli/         DSBoxCli
      net/         DSBoxNet, DSBoxNetFile, DSBoxNetHttp, DSBoxNetSocket
        tls/       DSBoxNetTls, DSBoxNetTlsLocalCa, DSBoxNetTlsSystemCa,
                   DSBoxNetTlsOpenSsl
      ui/          DSBoxSDL, DSBoxUi
        DSBoxSDL/  SDL3's own built DLL/.so (module-specific dependency
                   subfolder, nested under the module that bundles it)
      render/      DSBoxVulkan, DSBoxVulkanGui
    executables/   DSBoxUiDemo, DSBoxAdbTool, DSBoxLauncher ("dsbox")
    framework/
      mod/         DSBoxModRuntime.dll
      test/        every component's test binaries (DSBoxCoreTests,
                   DSBoxIniTests, ...), each with its own runtime DLLs
                   copied alongside it via dsbox_copy_runtime_dlls()
                   (see cmake/DSBoxCopyRuntimeDlls.cmake)
    plugins/       TestWindowMod (and any other MODULE-type target meant
                   to be loaded dynamically via ModuleLoader, never
                   implicitly linked)
    unsorted/      fallback for anything NOT given an explicit
                   dsbox_set_output_subdir() call -- should normally stay
                   empty; its presence after a build means a target was
                   missed in the top-level CMakeLists.txt's mapping.
  lib/             Windows .lib import libraries (CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  config/          see part 2 below -- a sibling of bin/, not nested
                   under bin/executables/
  shaders/
    dsbox_vulkan_gui/  DSBoxVulkanGui's compiled *.spv shaders -- a
                       top-level fin-out/ category of its own, a
                       sibling of fin-out/bin/, not copied next to the
                       DLL the way it was before the fin-out/ move
```

Every path that reaches one of these folders at runtime (executable
code resolving `configDir` or `shaderDirectory`, see part 2 and part 4
below) has to account for the executable itself sitting **two** levels
under `fin-out/` (`fin-out/bin/executables/<exe>`), not one -- this is
the detail that changed in every consumer when `fin-out/` was
introduced.

`DSBoxModLoader.exe` is emitted directly into the `fin-out/` root, outside
`bin/executables/`. It launches the game and loads
`bin/framework/mod/DSBoxModRuntime.dll`. Core, Config, and Ini remain in
their shared `bin/framework/core/` directory; the loader adds that directory
to the target process's DLL search path before loading the runtime.

The mapping is defined in one place: the block of
`dsbox_set_output_subdir(target subdir)` calls near the bottom of the
top-level `CMakeLists.txt`, right before `dsbox_apply_pch()`. See
`cmake/DSBoxOutputDirs.cmake` for what that function actually does
(and why it needs to set every per-config variant, not just the plain
property, for multi-config generators like Visual Studio).

## 2. Runtime DLL/.so resolution

### Windows

An executable's own implicit DLL imports are resolved by the OS loader
at process creation -- before any of our code runs, so a runtime
config setting can't influence them. The fix: the executable's
**direct** `DSBox::` dependencies are delay-loaded
(`/DELAYLOAD`, via `dsbox_delay_load()` in
`cmake/DSBoxDelayLoad.cmake`), deferring their resolution until first
use. That gives `main()` a window to call
`dsbox::bootstrapFrameworkSearchPaths()` (in
`dsbox_core/include/dsbox/bootstrap_paths.hpp`) first, which calls
`AddDllDirectory()` for every `bin/framework/<subsystem>/` subfolder
(plus any `[setup] additional_paths=` entries from a config file, if
one is passed in).

Only an executable's **direct** dependencies need `/DELAYLOAD` markup.
A dependency's own further dependencies (e.g. `DSBoxSDL.dll` implicitly
importing `SDL3.dll`) resolve correctly with no extra markup: by the
time `DSBoxSDL.dll` itself is loaded (already deferred past process
start by the delay-load on the executable's side),
`AddDllDirectory()` has already widened the process-wide DLL search
path, and that same search path is what resolves `SDL3.dll` too.

This same process-wide search path is also what a dynamically loaded
**plugin** (`TestWindowMod`, or a user mod loaded via
`dsbox::ModuleLoader::loadDynamicModule()`) resolves its own implicit
imports against -- so no separate plumbing was needed in
`module_loader.cpp` for plugins to find their dependencies either.

`bootstrapFrameworkSearchPaths()` must be the **literal first
statement** in `main()`, ahead of anything -- including printing a
version banner, since `dsbox::kVersionMajor` etc. (`version.hpp`) are
themselves data symbols pulled from the delay-loaded `DSBoxCore.dll`.

Framework hosts bootstrap search paths before using their dependencies.
The mod loader configures the target process's search path before loading
`DSBoxModRuntime.dll` from `bin/framework/mod/`.

**NOTE: this subsection is superseded below.** The runtime no longer
relies solely on the loader having done this -- see "DSBoxModRuntime
self-bootstrap" and "Loading DSBoxModRuntime directly via LuaJIT FFI".

Note that the `fin-out/` restructuring (part 1 above) did not change
any of this: `bin/`'s own internal relative structure
(`bin/executables/<exe>` to `bin/framework/<subsystem>/`) is the same
as before, just nested one level deeper overall, so neither
`defaultFrameworkSubfolders()`'s hardcoded `../framework/<subsystem>`
entries nor the `CMAKE_BUILD_RPATH` list below needed any changes.

### Linux / macOS

Handled entirely at link time via `RPATH` (`CMAKE_BUILD_RPATH` in the
top-level `CMakeLists.txt`): one `$ORIGIN/...` entry per framework
subfolder, from both the "I'm an executable in `bin/executables/`" and
"I'm a library in `bin/framework/net/tls/`" depths. No runtime step
needed; `bootstrapFrameworkSearchPaths()` is a no-op here.

## 2b. `bootstrapFrameworkSearchPaths()` takes the framework ROOT, not the caller's directory

As of this round, `dsbox::bootstrapFrameworkSearchPaths()`
(`dsbox_core/include/dsbox/bootstrap_paths.hpp`) takes the **framework
root** directory -- `fin-out/bin/framework/` itself, the folder that
directly contains `core/`, `scheduler/`, `ipc/`, ... -- not the caller's
own directory. Each caller computes that root relative to itself:

- The three executables (`fin-out/bin/executables/<exe>`) pass
  `executableDirectory() / ".." / "framework"`.
- `DSBoxModRuntime.dll` (`fin-out/bin/framework/mod/`) passes
  `ownModuleDirectory() / ".."`.

Both resolve to the same `fin-out/bin/framework/` in practice. Earlier,
the function hardcoded `"../framework/<subsystem>"` subfolder paths
relative to the caller, which only happened to be correct for the three
executables; `DSBoxModRuntime` computing the same relative paths from
its own directory landed on `fin-out/bin/framework/framework/<x>` (one
`framework` too many) and never found `DSBoxCore.dll`. New callers
should always pass the framework root, computed from their own known
position under `fin-out/`, never assume the default subfolder list is
relative to themselves.

## 2c. `DSBoxModRuntime` self-bootstrap

`DSBoxModRuntime.dll`'s own direct dependencies (`DSBoxCore`,
`DSBoxConfig`) are now delay-loaded too (see the
`dsbox_delay_load(DSBoxModRuntime ...)` call in the top-level
`CMakeLists.txt`), and `DSBoxRuntimeInitialize()`
(`dsbox_mod_runtime/src/runtime_entry.cpp`) calls
`dsbox::bootstrapFrameworkSearchPaths()` as the literal first thing it
does, exactly like the three executables' `main()`.

This used to be unnecessary -- `DSBoxModLoader` (the Steam-launch-option
shim, `dsbox_mod_loader/src/main.cpp`) called `SetDllDirectoryW()`
*remotely*, inside the target game's process, before `LoadLibrary`-ing
the runtime DLL into it (`setRemoteDllDirectory()`, see
`process_guard.hpp`), so the runtime itself never had to care. That
remote call still happens and is now simply redundant (harmless --
`AddDllDirectory()` adding the same directory twice is a no-op). The
self-bootstrap exists for the OTHER way this DLL gets loaded: directly,
by any host that is not `DSBoxModLoader` -- see the next section.

## 2d. Loading `DSBoxModRuntime` directly (no `DSBoxModLoader`) -- e.g. from a game's embedded LuaJIT

Some games (Scrap Mechanic is one real example) embed their own Lua/
LuaJIT runtime and expose `ffi.load()`/`ffi.cdef()` to mods, which lets
a mod load `DSBoxModRuntime.dll` directly, with no `DSBoxModLoader` or
any other DSBox process involved at all. Before this round, that
always failed: the DLL's own implicit imports (`DSBoxCore.dll`,
`DSBoxConfig.dll`, from `bin/framework/core/`) were resolved by the OS
loader at `LoadLibrary()` time, before any DSBox code had a chance to
widen the search path -- `LoadLibrary`/`ffi.load` would fail outright
(Windows error 126, `ERROR_MOD_NOT_FOUND`) or `DSBoxRuntimeInitialize`
would never even be reachable. The self-bootstrap in 2c fixes this: the
DLL's own imports are now delay-loaded, so `DSBoxRuntimeInitialize()`
gets to run and widen the search path before anything touches
`DSBoxCore`/`DSBoxConfig`.

**Minimal LuaJIT FFI usage, once the package is unpacked somewhere under
the game (e.g. `./BetterExtensions/DSBox/`):**

```lua
local ffi = require("ffi")

ffi.cdef[[
    int DSBoxRuntimeInitialize(void);  -- actually BOOL/WINAPI (stdcall);
                                        -- see the note below on x86 vs x64
    void* GetDsboxContext(void);        -- __cdecl
    void DSBoxRuntimeShutdown(void);    -- WINAPI (stdcall)
]]

-- Prefer an ABSOLUTE path over a relative one. A relative path is
-- resolved against the PROCESS's current working directory, which is
-- usually (not always -- depends on how the game/launcher starts it)
-- the game's install root; it is never resolved relative to this .lua
-- file's own location, and a third-party mod manager or overlay can
-- change it. Build an absolute path from whatever the game/mod loader
-- gives you as its own root instead of assuming CWD, if at all possible.
local dllPath = "./BetterExtensions/DSBox/bin/framework/mod/DSBoxModRuntime.dll"

local runtime = ffi.load(dllPath)

-- DSBoxModRuntime does NOT auto-initialize itself on load (there is no
-- DllMain-driven startup -- see runtime_entry.cpp's own top comment:
-- normally DSBoxModLoader calls this explicitly right after its own
-- LoadLibrary, which here is YOUR job instead).
if runtime.DSBoxRuntimeInitialize() == 0 then
    error("DSBoxModRuntime: DSBoxRuntimeInitialize failed")
end

-- Optional: a raw dsbox::Context* for native code (e.g. another plugin
-- .dll loaded via this runtime's own config/modules.ini) to pick up --
-- see the caveat below, this is NOT directly usable from Lua.
local ctx = runtime.GetDsboxContext()

-- Before unloading/releasing the DLL handle, stop DSBox modules and release
-- the runtime's Context (call outside DllMain and after init returns).
runtime.DSBoxRuntimeShutdown()
```

Things worth knowing before relying on this:

- **Calling convention.** `DSBoxRuntimeInitialize` is declared
  `BOOL WINAPI` (`__stdcall`); `GetDsboxContext` is plain `__cdecl`.
  On x86 these are genuinely different ABIs and LuaJIT's `cdef` would
  need `__stdcall` spelled out explicitly for the first one. On x64 --
  which is what Scrap Mechanic and virtually every modern Steam game
  ship -- there is only one calling convention, so the distinction is
  moot; the `cdef` above is fine as written for x64. Only revisit this
  if targeting a 32-bit host.
- **Initialization is NOT automatic.** `DSBoxRuntimeInitialize()` must
  be called explicitly after `ffi.load()` -- it starts a worker thread
  (see `runtimeMain()`) that loads `config/modules.ini` next to the DLL
  and starts whatever modules it lists, then waits for that work to
  finish before returning. Forgetting this call is the single most
  common way this looks "loaded but doing nothing".
- **Shutdown is explicit.** Call `DSBoxRuntimeShutdown()` after
  initialization has returned and before unloading the DLL. It joins the
  worker, stops modules, and releases the context. Call outside `DllMain`
  and only after host code has stopped using the raw pointer returned by
  `GetDsboxContext()`. The runtime cannot be initialized again after shutdown.
- **`GetDsboxContext()` returns a raw C++ pointer, not a Lua-usable
  object.** `dsbox::Context` is a real C++ class with virtual methods;
  LuaJIT FFI can hold the returned `void*` and pass it on to other
  *native* code (e.g. a plugin `.dll` built against `dsbox_core`'s
  headers, loaded via this runtime's own `modules.ini`), but Lua itself
  cannot call C++ virtual methods through it without a dedicated C API
  wrapper -- there isn't one today. If driving DSBox modules directly
  from Lua (not just native plugins) turns out to be a real need, that
  wrapper is the right next step, not working around `Context` from Lua.
- **Logging.** With no console (the host game's own), diagnostics go to
  `DSBoxModRuntime.log` next to the DLL (`bin/framework/mod/
  DSBoxModRuntime.log`) -- check that first if initialization silently
  does nothing.
- **This has nothing to do with the game's own working directory.**
  Everything above -- finding `DSBoxCore.dll`, finding `config/
  modules.ini`, finding plugin `.dll`s under `modules/` -- resolves from
  `DSBoxModRuntime.dll`'s own on-disk location (`GetModuleFileNameW` on
  itself), never from the process's current working directory. Placing
  `fin-out`'s contents under `./BetterExtensions/DSBox/` and loading
  `.../DSBox/bin/framework/mod/DSBoxModRuntime.dll` is correct regardless
  of what the game's CWD happens to be at the time.

## 2e. `BetterDSBox` is both a DSBox plugin AND a `require()`-able Lua module

`better_dsbox/src/better_dsbox.cpp` exports Lua bindings two different
ways from the same DLL, both reading the same static `dsbox::Context*`
that `ModuleManager` publishes via `onInit`:

- `BetterDSBox_RegisterLua(lua_State*)` -- for a NATIVE caller that
  already owns a `lua_State*` (an embedding host, or something that
  hooked the game's own Lua init natively). Sets a global `dsbox` table.
- `luaopen_BetterDSBox(lua_State*)` -- the standard Lua/LuaJIT module
  entry point. A Lua SCRIPT reaches it the normal way --
  `require("BetterDSBox")`, or directly via
  `package.loadlib(path, "luaopen_BetterDSBox")()` if `package.cpath`
  doesn't cover the DSBox install path -- and Lua's own require
  machinery supplies the correct `lua_State*` automatically, the way
  any genuine Lua C function receives it (unlike a plain `ffi.load()` +
  direct ABI call, which never gets one). Leaves the `dsbox` API table
  on the stack and returns 1, the normal luaopen contract, so
  `require()` caches it as `package.loaded["BetterDSBox"]`.

Both fail softly (no C++ exception ever crosses the Lua/C boundary) if
called before `ModuleManager` has run `onInit` on this plugin yet --
expected if Lua's `require("BetterDSBox")` races ahead of `modules.ini`
still loading on `DSBoxModRuntime`'s worker thread (see section 2c/2d).
For `luaopen_BetterDSBox` this is a real Lua error (`luaL_error`), so
`require()` does NOT cache the failure and a later `require("BetterDSBox")`
retries cleanly; callers should wrap early calls in `pcall`.

Because `BetterDSBox.dll` can now be loaded directly by Lua's `require`
-- with no `DSBoxModRuntime` involved at all, same as section 2d --
it bootstraps its own DLL search path itself (`bootstrapOwnSearchPath()`
in `better_dsbox.cpp`, the first thing either exported function does),
the same way `DSBoxModRuntime` does. This needs `DSBoxCore` delay-loaded
for the `BetterDSBox` target too (see the `dsbox_delay_load(BetterDSBox
DSBoxCore)` call in the top-level `CMakeLists.txt`) -- without it, a
direct `require("BetterDSBox")` with no prior `DSBoxModRuntime`
bootstrap would fail to resolve `DSBoxCore.dll` at `LoadLibrary` time,
the exact same bug class section 2d fixed for `DSBoxModRuntime` itself.
`BetterDSBox.dll` lives at `fin-out/bin/plugins/` -- the same depth
under `fin-out/` as an executable in `fin-out/bin/executables/<exe>` --
so it computes its framework root the same way they do:
`ownDir / ".." / "framework"`.

## 3. Configuration: consolidated `config/` tree

Previously each tool shipped its own config next to its own built exe
(`dsbox_adb_tool/config/default/config.ini` copied to
`<exe dir>/config/default/config.ini`, `launcher/example_modules.ini`
as a standalone example file). Now there's one tree, under the
`fin-out/` output root as a sibling of `fin-out/bin/` (not nested
under `bin/executables/`, and not reached via `$<TARGET_FILE_DIR:...>`
anymore -- `dsbox_adb_tool/CMakeLists.txt`'s POST_BUILD copy step
writes directly to `${DSBOX_OUTPUT_ROOT}/config/...`):

```
<repo root>/config/                  <-- source of truth, in git
  dsbox_adb_tool/default/config.ini  shipped defaults
  launcher/modules.ini.example       example module list (not auto-loaded)

<build>/fin-out/config/              <-- POST_BUILD-copied destination
  dsbox_adb_tool/default/config.ini
  dsbox_adb_tool/config.ini          (user overrides, created at runtime
                                       by PersistentConfigStore; not
                                       shipped)
```

`dsbox_adb_tool` and `launcher` each resolve their own `configDir` via
`--pathmode`:
- `--pathmode 0` (default): `<exe dir>/../../config/<tool name>` --
  the consolidated tree above. Two `..` steps, not one: the executable
  itself lives at `fin-out/bin/executables/<exe>`, and `config/` is a
  sibling of `fin-out/bin/`, so the path goes
  `executables -> bin -> fin-out -> config`.
- `--pathmode 1` (cwd) / `--pathmode 2 <path>` (explicit): unchanged,
  simply `<base>/config` -- these modes already hand full control of
  the directory layout to whoever passed them, so they're left alone.

`modulesDir` (where plugin `.dll`/`.so` paths in a `modules.ini` are
resolved from) is a separate concern and was not moved -- it still
tracks `baseDir` the same way it always did.

`dsbox_mod_runtime/example_modules.ini` remains as an example of the
runtime's `config/modules.ini` format; the config and module folders live
beside `DSBoxModRuntime.dll` under `bin/framework/mod/`.

The old per-tool config file locations
(`dsbox_adb_tool/config/default/config.ini`,
`launcher/example_modules.ini`) were left in place as short stub files
pointing here, rather than deleted outright -- safe to remove by hand.

## 4. `dsbox_vulkan_gui`'s compiled shaders

`dsbox_vulkan_gui/CMakeLists.txt` compiles `shaders/rect.{vert,frag}`
to SPIR-V via `glslc` and copies the result into
`${DSBOX_OUTPUT_ROOT}/shaders/dsbox_vulkan_gui/` -- a top-level
`fin-out/` category of its own (a sibling of `fin-out/bin/`), not
copied next to `DSBoxVulkanGui`'s own built binary the way it was
before the `fin-out/` move. `VulkanUiRenderer`'s constructor takes
that folder's path explicitly rather than hardcoding it, so each host
application resolves its own default. `dsbox_ui_demo/src/main.cpp` and
`dsbox_adb_tool/src/gui_mode.cpp` both do this the same way, from the
executable's own directory (`fin-out/bin/executables/<exe>` or
`fin-out/bin/framework/<x>/<exe>`): `executableDirectory() / ".." /
".." / "shaders" / "dsbox_vulkan_gui"` -- two `..` steps, the same
reasoning as `resolveConfigDir()` in part 3 above.
