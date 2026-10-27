#pragma once

#include <better_dsbox/api.hpp>

#include <dsbox/module.hpp>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace dsbox { class Context; }

namespace better_dsbox
{
    // Dynamically loaded DSBox plugin. ModuleManager calls onInit with the
    // host-owned Context before Lua clients can use its bindings.
    class BETTER_DSBOX_API BetterDSBox final : public dsbox::IModule
    {
    public:
        std::string_view name() const override { return "better_dsbox"; }
        std::vector<std::string> dependencies() const override
        {
            return { "dsbox.sdl.window" };
        }
        void onInit(dsbox::Context& context) override;
        void onShutdown(dsbox::Context& context) override;

        // Registers the LuaJIT-facing API on a guest-owned Lua state.
        // Every Lua closure resolves the one host-owned static Context
        // pointer, so all API tables use the same active Context.
        void registerLua(lua_State* state);

        static dsbox::Context* currentContext() noexcept;
        static void registerLuaForCurrentContext(lua_State* state);

    private:
        static std::atomic<dsbox::Context*> s_context;
        dsbox::Context* m_context = nullptr;
    };
}

// Guest entry point for a NATIVE host that already owns a lua_State*
// (e.g. an embedding application, or a separate injection that hooked
// the game's own Lua init) -- sets a GLOBAL `dsbox` table on it. Never
// accepts/creates a Context; reads the one set by ModuleManager's
// onInit. Not usable from a sandboxed Lua *script* itself: a plain
// ffi.load() + direct ABI call never gets a real lua_State* handed to
// it the way an actual Lua C call does.
extern "C" BETTER_DSBOX_PLUGIN_EXPORT int BetterDSBox_RegisterLua(lua_State* state);

// Standard Lua/LuaJIT `require("BetterDSBox")` entry point. This is the
// one a sandboxed Lua SCRIPT can actually use on its own: Lua's
// require()/package.loadlib() calls an exported `luaopen_<modname>`
// through the real Lua C-function calling convention, so Lua supplies
// the correct lua_State* itself -- no native bridge needed. Leaves the
// `dsbox` API table on the stack and returns 1, per the normal luaopen
// contract (require() then caches that return value in
// package.loaded["BetterDSBox"]). Same underlying static Context as
// BetterDSBox_RegisterLua/onInit -- this does NOT create a second
// BetterDSBox module instance, it only needs ModuleManager to have
// already called onInit via DSBox_CreateModule (normally through
// modules.ini) at some point, possibly after this is first called --
// see this function's own .cpp comment for the resulting race and how
// it's handled.
extern "C" BETTER_DSBOX_PLUGIN_EXPORT int luaopen_BetterDSBox(lua_State* state);
