local better, directory = ...

local ffi = require("ffi")
DSBOX_HANDLES = {}

local RUNTIME_DLL = directory .. "/bin/framework/mod/DSBoxModRuntime.dll"

local BETTER_DSBOX_DLL = directory .. "/bin/plugins/BetterDSBox.dll"

ffi.cdef[[
    int DSBoxRuntimeInitialize(void);  // BOOL WINAPI; cdecl==stdcall on x64
    void DSBoxRuntimeShutdown(void);  // BOOL WINAPI; cdecl==stdcall on x64
]]

local runtime = assert(ffi.load(RUNTIME_DLL))
DSBOX_HANDLES.runtime = runtime
if runtime.DSBoxRuntimeInitialize() == 0 then
    error("DSBox: DSBoxRuntimeInitialize() failed -- check DSBoxModRuntime.log next to the DLL")
end
bext.registerHandler_exit(function()
    runtime.DSBoxRuntimeShutdown()
end)


print("[DSBox] runtime initialize requested; module loading continues on a background thread")

package.preload["better_dsbox"] = function()
    local loader = assert(package.loadlib(BETTER_DSBOX_DLL, "luaopen_BetterDSBox"))
    return loader() -- actually calls luaopen_BetterDSBox(L) and returns its table
end

local dsbox
local ok, result = pcall(require, "better_dsbox")
if ok then
    dsbox = result
    DSBOX_HANDLES.api = result
    print("[DSBox] BetterDSBox API ready:", dsbox.context_ready())
else
    print("[DSBox] require(\"better_dsbox\") failed: " .. tostring(result))
end

local window = require "dsbox_sdl"
better.window = window