#include <better_dsbox/better_dsbox.hpp>

#include <dsbox/bootstrap_paths.hpp>
#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>
#include <dsbox_sdl/window_module.hpp>

#include <SDL3/SDL.h>

extern "C"
{
    #include <lua.h>
    #include <lauxlib.h>
}

#include <filesystem>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace better_dsbox
{
    std::atomic<dsbox::Context*> BetterDSBox::s_context{ nullptr };

    namespace
    {
        dsbox::Context& luaContext(lua_State* state)
        {
            auto* context = BetterDSBox::currentContext();
            if (!context)
                luaL_error(state, "BetterDSBox is not initialized");
            return *context;
        }

        int contextReady(lua_State* state)
        {
            (void)luaContext(state);
            lua_pushboolean(state, 1);
            return 1;
        }

        int hasModule(lua_State* state)
        {
            auto& context = luaContext(state);
            const char* name = luaL_checkstring(state, 1);
            auto* modules = context.modules();
            lua_pushboolean(state, modules && modules->has(name));
            return 1;
        }

        int moduleNames(lua_State* state)
        {
            auto* modules = luaContext(state).modules();
            if (!modules)
                return luaL_error(state, "BetterDSBox Context has no bound ModuleManager");

            const auto names = modules->moduleNames();
            lua_createtable(state, static_cast<int>(names.size()), 0);
            int index = 1;
            for (const auto& name : names)
            {
                lua_pushlstring(state, name.data(), name.size());
                lua_rawseti(state, -2, index++);
            }
            return 1;
        }

        dsbox_sdl::WindowModule& sdlWindowModule(lua_State* state)
        {
            auto* modules = luaContext(state).modules();
            if (!modules)
                luaL_error(state, "BetterDSBox Context has no bound ModuleManager");
            auto* base = modules->find("dsbox.sdl.window");
            if (!base)
                luaL_error(state, "ModuleManager has no registered module named dsbox.sdl.window");
            auto* window = dynamic_cast<dsbox_sdl::WindowModule*>(base);
            if (!window)
                luaL_error(state, "Module dsbox.sdl.window is registered, but is not a WindowModule instance");
            return *window;
        }

        dsbox_sdl::WindowHandle checkedWindowHandle(lua_State* state)
        {
            const lua_Integer value = luaL_checkinteger(state, 1);
            if (value <= 0)
                luaL_argerror(state, 1, "window handle must be a positive integer");
            return static_cast<dsbox_sdl::WindowHandle>(value);
        }

        std::uint8_t checkedColor(lua_State* state, int index, std::uint8_t fallback = 255)
        {
            if (lua_isnoneornil(state, index))
                return fallback;
            const lua_Integer value = luaL_checkinteger(state, index);
            if (value < 0 || value > 255)
                luaL_argerror(state, index, "color channel must be between 0 and 255");
            return static_cast<std::uint8_t>(value);
        }

        int createWindow(lua_State* state)
        {
            dsbox_sdl::WindowOptions options;
            if (!lua_isnoneornil(state, 1))
                options.title = luaL_checkstring(state, 1);
            options.width = static_cast<int>(luaL_optinteger(state, 2, 800));
            options.height = static_cast<int>(luaL_optinteger(state, 3, 600));
            if (options.width <= 0 || options.height <= 0)
                return luaL_error(state, "window dimensions must be positive");

            const auto handle = sdlWindowModule(state).createWindow(std::move(options));
            if (handle == dsbox_sdl::kInvalidWindowHandle)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, static_cast<lua_Integer>(handle));
            return 1;
        }

        int closeWindow(lua_State* state)
        {
            sdlWindowModule(state).closeWindow(checkedWindowHandle(state));
            return 0;
        }

        int setWindowTitle(lua_State* state)
        {
            sdlWindowModule(state).setWindowTitle(
                checkedWindowHandle(state), luaL_checkstring(state, 2));
            return 0;
        }

        int getWindowTitle(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            std::string title;
            bool found = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                const char* value = SDL_GetWindowTitle(window);
                if (value)
                {
                    title = value;
                    found = true;
                }
            });
            if (!found)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushlstring(state, title.data(), title.size());
            return 1;
        }

        int getWindowSize(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            int width = 0, height = 0;
            if (!sdlWindowModule(state).windowSize(handle, width, height))
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, width);
            lua_pushinteger(state, height);
            return 2;
        }

        int getWindowPosition(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            int x = 0, y = 0;
            bool found = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                SDL_GetWindowPosition(window, &x, &y);
                found = true;
            });
            if (!found)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushinteger(state, x);
            lua_pushinteger(state, y);
            return 2;
        }

        int isWindowVisible(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            bool found = false;
            bool visible = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                found = true;
                visible = (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) == 0;
            });
            if (!found)
            {
                lua_pushnil(state);
                return 1;
            }
            lua_pushboolean(state, visible);
            return 1;
        }

        int isWindowValid(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            lua_pushboolean(state, sdlWindowModule(state).isWindowValid(handle));
            return 1;
        }

        int setWindowSize(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            const int width = static_cast<int>(luaL_checkinteger(state, 2));
            const int height = static_cast<int>(luaL_checkinteger(state, 3));
            if (width <= 0 || height <= 0)
                return luaL_error(state, "window dimensions must be positive");
            bool called = false;
            bool success = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                called = true;
                success = SDL_SetWindowSize(window, width, height);
            });
            lua_pushboolean(state, called && success);
            return 1;
        }

        int setWindowPosition(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            const int x = static_cast<int>(luaL_checkinteger(state, 2));
            const int y = static_cast<int>(luaL_checkinteger(state, 3));
            bool called = false;
            bool success = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                called = true;
                success = SDL_SetWindowPosition(window, x, y);
            });
            lua_pushboolean(state, called && success);
            return 1;
        }

        int showWindow(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            bool called = false;
            bool success = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                called = true;
                success = SDL_ShowWindow(window);
            });
            lua_pushboolean(state, called && success);
            return 1;
        }

        int hideWindow(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            bool called = false;
            bool success = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                called = true;
                success = SDL_HideWindow(window);
            });
            lua_pushboolean(state, called && success);
            return 1;
        }

        struct DrawCommand
        {
            enum class Kind { Clear, FillRect, Line } kind;
            float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            std::uint8_t r = 255, g = 255, b = 255, a = 255;
        };

        int render(lua_State* state)
        {
            const auto handle = checkedWindowHandle(state);
            luaL_checktype(state, 2, LUA_TTABLE);
            const int count = static_cast<int>(lua_objlen(state, 2));
            std::vector<DrawCommand> commands;
            commands.reserve(static_cast<std::size_t>(count));

            for (int index = 1; index <= count; ++index)
            {
                lua_rawgeti(state, 2, index);
                luaL_checktype(state, -1, LUA_TTABLE);

                lua_getfield(state, -1, "type");
                const char* type = luaL_checkstring(state, -1);
                DrawCommand command;
                if (std::string(type) == "clear") command.kind = DrawCommand::Kind::Clear;
                else if (std::string(type) == "rect") command.kind = DrawCommand::Kind::FillRect;
                else if (std::string(type) == "line") command.kind = DrawCommand::Kind::Line;
                else return luaL_error(state, "draw command type must be clear, rect, or line");
                lua_pop(state, 1);

                auto numberField = [state](const char* name, float& output, bool required)
                {
                    lua_getfield(state, -1, name);
                    if (lua_isnil(state, -1) && !required)
                    {
                        lua_pop(state, 1);
                        return;
                    }
                    output = static_cast<float>(luaL_checknumber(state, -1));
                    lua_pop(state, 1);
                };
                if (command.kind == DrawCommand::Kind::FillRect)
                {
                    numberField("x", command.x1, true);
                    numberField("y", command.y1, true);
                    numberField("w", command.x2, true);
                    numberField("h", command.y2, true);
                }
                else if (command.kind == DrawCommand::Kind::Line)
                {
                    numberField("x1", command.x1, true);
                    numberField("y1", command.y1, true);
                    numberField("x2", command.x2, true);
                    numberField("y2", command.y2, true);
                }
                auto colorField = [state](const char* name, std::uint8_t& output)
                {
                    lua_getfield(state, -1, name);
                    if (!lua_isnil(state, -1))
                    {
                        const lua_Integer value = luaL_checkinteger(state, -1);
                        if (value < 0 || value > 255)
                            luaL_error(state, "color channel must be between 0 and 255");
                        output = static_cast<std::uint8_t>(value);
                    }
                    lua_pop(state, 1);
                };
                colorField("r", command.r);
                colorField("g", command.g);
                colorField("b", command.b);
                colorField("a", command.a);
                commands.push_back(command);
                lua_pop(state, 1);
            }

            bool called = false;
            bool success = false;
            sdlWindowModule(state).withNativeWindow(handle, [&](SDL_Window* window)
            {
                called = true;
                SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
                if (!renderer)
                    return;
                success = true;
                for (const auto& command : commands)
                {
                    if (!SDL_SetRenderDrawColor(renderer, command.r, command.g,
                            command.b, command.a))
                    {
                        success = false;
                        break;
                    }
                    switch (command.kind)
                    {
                        case DrawCommand::Kind::Clear:
                            success = SDL_RenderClear(renderer);
                            break;
                        case DrawCommand::Kind::FillRect:
                        {
                            SDL_FRect rect{ command.x1, command.y1, command.x2, command.y2 };
                            success = SDL_RenderFillRect(renderer, &rect);
                            break;
                        }
                        case DrawCommand::Kind::Line:
                            success = SDL_RenderLine(renderer, command.x1, command.y1,
                                command.x2, command.y2);
                            break;
                    }
                    if (!success) break;
                }
                if (success) success = SDL_RenderPresent(renderer);
                SDL_DestroyRenderer(renderer);
            });
            lua_pushboolean(state, called && success);
            return 1;
        }


        void addFunction(lua_State* state, const char* name, lua_CFunction function)
        {
            lua_pushcfunction(state, function);
            lua_setfield(state, -2, name);
        }
    }

    // Shared by BetterDSBox_RegisterLua (sets a global) and
    // luaopen_BetterDSBox (returns via the stack, the normal luaopen
    // contract) -- builds the exact same API table either way, from
    // whatever Context ModuleManager's onInit() already published.
    // Leaves the table on top of the stack; does NOT touch globals.
    // Throws std::logic_error if onInit() hasn't run yet (e.g. a Lua
    // script's require("BetterDSBox") winning the race against
    // modules.ini still being loaded on DSBoxModRuntime's worker
    // thread) -- both callers below (and luaopen_BetterDSBox, further
    // down in this file, OUTSIDE this namespace -- hence this being a
    // named-namespace function, not left in the anonymous namespace
    // above, which qualified lookup from outside cannot see) turn that
    // into a normal, catchable failure for their own side: a Lua error
    // for luaopen_BetterDSBox, so require() does NOT cache the module
    // and a later require() retries; a plain `return 0` for the native
    // BetterDSBox_RegisterLua entry point.
    void pushApiTable(lua_State* state)
    {
        dsbox::Context* context = BetterDSBox::currentContext();
        if (!context)
            throw std::logic_error("BetterDSBox has not been initialized by ModuleManager yet");
        if (!context->modules())
            throw std::logic_error("BetterDSBox Context has no bound ModuleManager");

        lua_newtable(state);
        addFunction(state, "context_ready", contextReady);
        addFunction(state, "has_module", hasModule);
        addFunction(state, "module_names", moduleNames);
        lua_newtable(state);
        addFunction(state, "create_window", createWindow);
        addFunction(state, "close_window", closeWindow);
        addFunction(state, "set_title", setWindowTitle);
        addFunction(state, "get_title", getWindowTitle);
        addFunction(state, "get_size", getWindowSize);
        addFunction(state, "get_position", getWindowPosition);
        addFunction(state, "is_visible", isWindowVisible);
        addFunction(state, "is_valid", isWindowValid);
        addFunction(state, "set_size", setWindowSize);
        addFunction(state, "set_position", setWindowPosition);
        addFunction(state, "show", showWindow);
        addFunction(state, "hide", hideWindow);
        addFunction(state, "render", render);
        lua_setfield(state, -2, "sdl");
    }

    void BetterDSBox::onInit(dsbox::Context& context)
    {
        dsbox::Context* expected = nullptr;
        if (!s_context.compare_exchange_strong(expected, &context,
                std::memory_order_acq_rel, std::memory_order_acquire) && expected != &context)
            throw std::logic_error("BetterDSBox is already initialized for a different DSBox Context");
        m_context = &context;
    }

    void BetterDSBox::onShutdown(dsbox::Context& context)
    {
        dsbox::Context* expected = &context;
        s_context.compare_exchange_strong(expected, nullptr,
            std::memory_order_acq_rel, std::memory_order_acquire);
        if (m_context == &context)
            m_context = nullptr;
    }

    dsbox::Context* BetterDSBox::currentContext() noexcept
    {
        return s_context.load(std::memory_order_acquire);
    }

    void BetterDSBox::registerLuaForCurrentContext(lua_State* state)
    {
        if (!state)
            throw std::invalid_argument("BetterDSBox registration requires a non-null lua_State");

        pushApiTable(state); // throws if onInit() hasn't run yet
        lua_setglobal(state, "dsbox");
    }

    void BetterDSBox::registerLua(lua_State* state)
    {
        registerLuaForCurrentContext(state);
    }
}

extern "C"
{
    BETTER_DSBOX_PLUGIN_EXPORT dsbox::IModule* DSBox_CreateModule()
    {
        return new better_dsbox::BetterDSBox();
    }

    BETTER_DSBOX_PLUGIN_EXPORT void DSBox_DestroyModule(dsbox::IModule* module)
    {
        delete module;
    }
}

namespace
{
#if defined(_WIN32)
    // Shared helper: the HMODULE of the DLL `exportAddress` (some
    // function's own address) belongs to -- nullptr if that can't be
    // determined. GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT: this
    // does not pin/leak a reference, it just resolves an address to its
    // owning module, same as the original BetterDSBox_RegisterLua check.
    HMODULE ownModuleHandle(void* exportAddress)
    {
        HMODULE module = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(exportAddress), &module);
        return module;
    }
#endif

    // Shared by BetterDSBox_RegisterLua and luaopen_BetterDSBox: confirms
    // `exportAddress` (the calling function's own address) resolves back
    // to the SAME already-loaded DLL instance ModuleManager created a
    // BetterDSBox C++ object against -- i.e. that this isn't some other,
    // independently loaded copy of the file (a second LoadLibrary of the
    // exact same path just bumps the refcount and returns the SAME
    // HMODULE on Windows, so this check only ever fails for a genuinely
    // different file/copy).
    bool belongsToLoadedBetterDSBox(void* exportAddress)
    {
#if defined(_WIN32)
        HMODULE module = ownModuleHandle(exportAddress);
        return module != nullptr && ::GetProcAddress(module, "DSBox_CreateModule") != nullptr;
#else
        Dl_info info{};
        if (dladdr(exportAddress, &info) == 0 || !info.dli_fname)
            return false;
        void* module = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
        if (!module)
            return false;
        dlclose(module);
        return true;
#endif
    }

    // Widens the process's DLL search path to cover every
    // bin/framework/<subsystem>/ subfolder, exactly like the three
    // executables' main() and DSBoxModRuntime's own
    // DSBoxRuntimeInitialize() do -- see dsbox/bootstrap_paths.hpp's top
    // comment. Needed here for the SAME reason it's needed in
    // DSBoxModRuntime: BetterDSBox.dll can now be loaded two ways --
    // (a) via ModuleManager (DSBoxModRuntime already bootstrapped the
    // process by the time that happens), or (b) directly, by Lua's own
    // require("BetterDSBox")/package.loadlib(), which can race ahead of
    // (a) or happen with no DSBoxModRuntime involved at all. BetterDSBox
    // links DSBox::Core, DSBox::SDL and SDL3 directly (see this
    // directory's CMakeLists.txt), so without this, path (b) could fail
    // to resolve those framework DLLs at load time. Their imports are
    // delay-loaded, leaving a window to widen the search path before
    // the first call into them.
    void bootstrapOwnSearchPath(void* exportAddress)
    {
#if defined(_WIN32)
        HMODULE module = ownModuleHandle(exportAddress);
        if (!module)
            return; // nothing to do if we can't even find ourselves

        wchar_t buffer[MAX_PATH];
        DWORD length = ::GetModuleFileNameW(module, buffer, MAX_PATH);
        if (length == 0 || length == MAX_PATH)
            return;

        // BetterDSBox.dll lives at fin-out/bin/plugins/ -- the same
        // depth under fin-out/ as an executable at
        // fin-out/bin/executables/<exe> (both are direct children of
        // bin/), so the framework root is reached the same way: one
        // ".." up to bin/, then into "framework".
        std::filesystem::path selfDir = std::filesystem::path(buffer).parent_path();
        dsbox::bootstrapFrameworkSearchPaths((selfDir / ".." / "framework").string());
#else
        (void)exportAddress; // no-op: RPATH already handles this on Linux/macOS
#endif
    }
}

extern "C" BETTER_DSBOX_PLUGIN_EXPORT int BetterDSBox_RegisterLua(lua_State* state)
{
    // See bootstrapOwnSearchPath()'s own comment: must run before
    // anything below touches DSBoxCore (registerLuaForCurrentContext
    // does, via BetterDSBox::currentContext()'s Context pointer use
    // further down the line). Harmless if DSBoxModRuntime already did
    // this for the process -- AddDllDirectory() just adds the same
    // directory twice.
    bootstrapOwnSearchPath(reinterpret_cast<void*>(&BetterDSBox_RegisterLua));

    try
    {
        if (!belongsToLoadedBetterDSBox(reinterpret_cast<void*>(&BetterDSBox_RegisterLua)))
            return 0;

        better_dsbox::BetterDSBox::registerLuaForCurrentContext(state);
    }
    catch (...)
    {
        return 0;
    }
    return 1;
}

// require("BetterDSBox") entry point -- see this function's own doc
// comment in better_dsbox.hpp for why this is the path a sandboxed Lua
// SCRIPT should actually use (as opposed to BetterDSBox_RegisterLua,
// which needs a native caller that already owns a lua_State*).
//
// Lua's require()/package.loadlib() calls this exactly like any other
// lua_CFunction -- `state` here is the real, correct Lua state, supplied
// by Lua itself, not something this code has to go find. On success,
// per the standard luaopen_* contract, the `dsbox` API table is left on
// top of the stack and this returns 1; require() then caches that
// return value as package.loaded["BetterDSBox"] and hands it back to
// the caller -- exactly what `local dsbox = require("BetterDSBox")`
// expects.
//
// A C++ exception must never unwind across this boundary back into
// Lua's own C call machinery (undefined behavior at best, a hard crash
// at worst) -- every failure path here goes through luaL_error()
// instead, which uses Lua's own (longjmp-based) error mechanism. A
// luaL_error() call never returns, so there is no explicit `return`
// after it; the `return 1` at the bottom is only reached on success.
// Because this ends in a genuine Lua error rather than a silent "return
// 0", require() does NOT cache a failed attempt in package.loaded, so a
// later require("BetterDSBox") call -- e.g. once ModuleManager has
// finished loading BetterDSBox via modules.ini on DSBoxModRuntime's
// worker thread -- tries again from scratch rather than being stuck
// with a cached failure.
extern "C" BETTER_DSBOX_PLUGIN_EXPORT int luaopen_BetterDSBox(lua_State* state)
{
    // MUST be the literal first thing this does -- see
    // bootstrapOwnSearchPath()'s own comment above. This is the path
    // most likely to run with NO DSBoxModRuntime involved at all (a Lua
    // script requiring "BetterDSBox" on its own), so it cannot assume
    // anyone else has widened the process's DLL search path yet.
    bootstrapOwnSearchPath(reinterpret_cast<void*>(&luaopen_BetterDSBox));

    if (!belongsToLoadedBetterDSBox(reinterpret_cast<void*>(&luaopen_BetterDSBox)))
        return luaL_error(state, "BetterDSBox: this DLL copy was not loaded as a DSBox module");

    try
    {
        better_dsbox::pushApiTable(state);
    }
    catch (const std::exception& error)
    {
        return luaL_error(state, "BetterDSBox: %s", error.what());
    }

    return 1;
}
