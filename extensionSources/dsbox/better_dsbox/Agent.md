# BetterDSBox

BetterDSBox is an optional, dynamically loaded DSBox plugin and LuaJIT
binding layer. `ModuleManager` creates it via `DSBox_CreateModule()` and
passes the host `dsbox::Context` to `onInit`; the plugin stores a non-owning
pointer. A second initialization with a different Context is rejected.

The DLL exposes Lua in TWO ways, both reading the same static Context
pointer set by `onInit`:

- `BetterDSBox_RegisterLua(lua_State*)` -- for a NATIVE host that already
  owns a `lua_State*` (an embedding application, or a separate injection
  that hooked the game's own Lua init). Sets a global `dsbox` table on
  the given state.
- `luaopen_BetterDSBox(lua_State*)` -- the standard Lua/LuaJIT module
  entry point, for `require("BetterDSBox")` /
  `package.loadlib(path, "luaopen_BetterDSBox")` called directly from a
  Lua SCRIPT (e.g. a game's own sandboxed LuaJIT with FFI access, no
  native bridge needed -- Lua's own require machinery supplies the
  correct `lua_State*` automatically, the way any real Lua C function
  receives it). Leaves the `dsbox` table on the stack and returns 1, per
  the normal luaopen contract, instead of setting a global.

Both expose the same `dsbox` table: `context_ready()`, `has_module(name)`,
and `module_names()`. Both fail gracefully (return 0 / raise a Lua error,
never letting a C++ exception cross the Lua/C boundary) if called before
`ModuleManager` has run `onInit` on this plugin -- normal if
`require("BetterDSBox")` races ahead of `modules.ini` still being loaded
on another thread; a Lua-side retry (a plain `pcall(require, ...)` loop)
is expected and, for `luaopen_BetterDSBox`, actually re-runs on the next
`require()` since a failed loader is never cached in `package.loaded`.

`BetterDSBox.dll` can now be loaded two different ways -- via
`ModuleManager` (after `DSBoxModRuntime` has already widened the
process's DLL search path) or directly via Lua's own `require`, which
can run with no `DSBoxModRuntime` involved at all -- so it bootstraps
its own DLL search path itself, the first thing either exported
function does (see `bootstrapOwnSearchPath()` in `better_dsbox.cpp`, and
`DSBox::Core` being delay-loaded for this target in the top-level
`CMakeLists.txt`). Without that, a direct `require("BetterDSBox")` with
no `DSBoxModRuntime` already having bootstrapped the process would fail
to resolve `DSBoxCore.dll` at `LoadLibrary` time -- the same bug class
`docs/BUILD_LAYOUT.md` section 2d documents for `DSBoxModRuntime` itself.

LuaJIT is discovered optionally; without its headers and library, the
rest of DSBox still configures normally.

Lua also receives `dsbox.sdl`. It uses the existing
`dsbox.sdl.window` module for window creation, close, and title operations;
BetterDSBox performs resize, position, show/hide, and frame rendering inside
`withNativeWindow()` callbacks so SDL work remains on the window control
thread. `render(handle, commands)` accepts an array of `clear`, `rect`, and
`line` command tables and presents the frame before returning. Its renderer
is temporary for each frame, so BetterDSBox does not retain native SDL
pointers after the callback.

Window information is available through `get_title(handle)`,
`get_size(handle)`, `get_position(handle)`, and `is_visible(handle)`. The size
and position functions return two numbers; these queries return `nil` when the
handle is no longer valid. Visibility reports whether the SDL window is
hidden, not whether another window obscures it.
Use `is_valid(handle)` to check explicitly whether a handle still refers to an
open window; it returns a boolean.

Example:

```lua
local dsbox = require("BetterDSBox")
local sdl = dsbox.sdl
local window = assert(sdl.create_window("Lua window", 800, 600))
local title = sdl.get_title(window)
local width, height = sdl.get_size(window)
local x, y = sdl.get_position(window)
local visible = sdl.is_visible(window)
local valid = sdl.is_valid(window)
sdl.render(window, {
    { type = "clear", r = 24, g = 28, b = 36 },
    { type = "rect", x = 40, y = 40, w = 160, h = 90, r = 80, g = 180, b = 255 },
    { type = "line", x1 = 40, y1 = 160, x2 = 200, y2 = 160, r = 255, g = 255, b = 255 },
})
```
