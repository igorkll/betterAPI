// DSBox runtime DLL loaded by DSBoxModLoader into the target game's process.
//
// Initialization is explicitly invoked by the loader after LoadLibraryW
// returns, so it runs outside the loader lock. It starts a worker and waits
// until the runtime and configured modules finish loading before returning.
//
// Clean shutdown is explicit and must run outside the Windows loader lock.

#include <dsbox_mod_runtime/api.hpp>

#include <dsbox/bootstrap_paths.hpp>
#include <dsbox/context.hpp>
#include <dsbox/module_manager.hpp>
#include <dsbox/version.hpp>

#include <dsbox_config/module_config_loader.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    struct RuntimeState
    {
        // The EXE's normal startup routine is not run when Windows maps it
        // as a module, so initialize C++ state lazily on first export call.
        std::unique_ptr<dsbox::Context> context;
        std::atomic<dsbox::Context*> publishedContext{ nullptr };
        std::unique_ptr<dsbox::ModuleManager> modules;
        std::ofstream log;
        std::mutex logMutex;
        std::atomic_bool started{ false };
        std::atomic_bool stopping{ false };
        std::mutex initializationMutex;
        std::condition_variable initializationChanged;
        bool initializationComplete = false;
        bool initializationSucceeded = false;
        std::thread worker;
    };

    RuntimeState& state()
    {
        static RuntimeState value;
        return value;
    }

    // GUI games typically have no console and no visible stdout/stderr,
    // so this runtime keeps its own small log file in the DSBox output root
    // rather than printf'ing into the void.
    void logLine(const std::string& text)
    {
        auto& runtime = state();
        std::lock_guard<std::mutex> lock(runtime.logMutex);

        if (runtime.log.is_open())
        {
            runtime.log << text << std::endl;
        }
    }

    std::filesystem::path ownModuleDirectory(HMODULE selfModule)
    {
        wchar_t buffer[MAX_PATH];
        DWORD length = ::GetModuleFileNameW(selfModule, buffer, MAX_PATH);

        if (length == 0 || length == MAX_PATH)
            throw std::runtime_error("Failed to determine the embedded runtime's own path");

        return std::filesystem::path(buffer).parent_path();
    }

    std::string readFileOrEmpty(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);

        if (!file)
            return {};

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Runs on the dedicated worker thread started by DSBoxRuntimeInitialize.
    bool runtimeMain(HMODULE selfModule)
    {
        std::filesystem::path baseDir;

        try
        {
            // DSBoxModRuntime.dll is emitted to <root>/bin/framework/mod/.
            // Runtime data (config/, modules/, shaders/) lives at <root>,
            // alongside bin/, so walk up three levels from the DLL directory.
            baseDir = (ownModuleDirectory(selfModule) / ".." / ".." / "..").lexically_normal();
        }
        catch (const std::exception&)
        {
            return false; // can't even find ourselves; nothing more to do
        }

        std::filesystem::path logPath = baseDir / "DSBoxModRuntime.log";
        {
            auto& runtime = state();
            std::lock_guard<std::mutex> lock(runtime.logMutex);
            runtime.log.open(logPath, std::ios::out | std::ios::app);
        }

        logLine("DSBox " + std::to_string(dsbox::versionMajor()) + "." +
            std::to_string(dsbox::versionMinor()) + "." +
            std::to_string(dsbox::versionPatch()) + " mod runtime starting");
        logLine("Base directory: " + baseDir.string());

        auto& runtime = state();
        runtime.context = std::make_unique<dsbox::Context>();
        runtime.publishedContext.store(runtime.context.get(), std::memory_order_release);
        runtime.modules = std::make_unique<dsbox::ModuleManager>(*runtime.context);

        // Anything linked statically into THIS dll (there are none by
        // default -- a modpack is expected to ship its modules as
        // separate .dll files listed in config/modules.ini instead, so
        // a mod author never needs to relink this runtime).
        runtime.modules->registerStaticModules();

        std::filesystem::path configDir = baseDir / "config";
        std::filesystem::path modulesDir = baseDir / "bin";
        std::filesystem::path modulesIniPath = configDir / "modules.ini";

        if (std::filesystem::exists(modulesIniPath))
        {
            std::string iniText = readFileOrEmpty(modulesIniPath);
            auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
                *runtime.modules, iniText, modulesDir.string());

            for (const auto& result : report.results)
            {
                if (!result.enabled)
                    logLine("[config] '" + result.alias + "' skipped (disabled)");
                else if (result.loaded)
                    logLine("[config] '" + result.alias + "' loaded module '" +
                        result.moduleName + "' from " + result.path);
                else
                    logLine("[config] '" + result.alias + "' failed: " + result.error);
            }
        }
        else
        {
            logLine("No config/modules.ini found under the DSBox output root -- nothing to load");
        }

        try
        {
            // Skip (rather than abort) a module that fails to start: a
            // single broken mod must never take the whole game process
            // down with it.
            runtime.modules->startAll(
                [](std::string_view moduleName, const std::exception& error)
                {
                    logLine(
                        "[module] '" + std::string(moduleName) + "' failed to start: " +
                        error.what() + " -- skipping it");
                    return dsbox::ModuleErrorAction::Skip;
                });
        }
        catch (const dsbox::ModuleError& error)
        {
            logLine(std::string("Failed to start modules: ") + error.what());
            return false;
        }

        logLine("DSBox mod runtime ready");
        return true;
    }
}

extern "C"
{
    DSBOX_MOD_RUNTIME_API dsbox::Context* __cdecl GetDsboxContext()
    {
        auto& runtime = state();
        if (runtime.stopping.load(std::memory_order_acquire))
            return nullptr;
        return runtime.publishedContext.load(std::memory_order_acquire);
    }

    // Call after DSBoxRuntimeInitialize returns, and always outside DllMain.
    // The worker is joined before module code is unloaded by stopAll().
    DSBOX_MOD_RUNTIME_API void WINAPI DSBoxRuntimeShutdown()
    {
        auto& runtime = state();
        runtime.stopping.store(true, std::memory_order_release);
        if (runtime.worker.joinable())
            runtime.worker.join();

        runtime.publishedContext.store(nullptr, std::memory_order_release);
        if (runtime.modules)
        {
            runtime.modules->stopAll();
            runtime.modules.reset();
        }
        runtime.context.reset();
        {
            std::lock_guard<std::mutex> lock(runtime.logMutex);
            if (runtime.log.is_open())
                runtime.log.close();
        }
        runtime.stopping.store(false, std::memory_order_release);
    }

    // MUST be the literal first thing this export does, before anything
    // touches a DSBoxCore/DSBoxConfig symbol (including runtimeMain()'s
    // own dsbox::Context construction, further down on the worker
    // thread) -- see dsbox/bootstrap_paths.hpp's own top comment. This
    // is only possible because DSBoxCore/DSBoxConfig are delay-loaded
    // for this target (see the top-level CMakeLists.txt's
    // dsbox_delay_load(DSBoxModRuntime ...) call): their imports are
    // NOT resolved at LoadLibrary() time the way an ordinary
    // (non-delay-loaded) dependency's would be, so there is still a
    // window here -- after the DLL is loaded but before first use -- to
    // widen the process's own DLL search path ourselves.
    //
    // Unlike the three executables (DSBoxUiDemo/DSBoxAdbTool/
    // DSBoxLauncher), which can always assume they ARE the process and
    // so bootstrap unconditionally, this runtime can be reached two
    // different ways:
    //   1. Injected by DSBoxModLoader, which ALSO calls
    //      SetDllDirectoryW() remotely in the target process before
    //      LoadLibrary -- redundant with what we do here, but harmless
    //      (AddDllDirectory() just adds the same directory twice).
    //   2. Loaded directly by any other host that can LoadLibrary a
    //      DLL and call an extern "C" export by name -- most notably a
    //      game's own embedded Lua/LuaJIT via FFI's ffi.load() +
    //      ffi.C.DSBoxRuntimeInitialize(), which is NOT DSBoxModLoader
    //      and never configures anything for us. This runtime must be
    //      fully self-sufficient for that path, hence bootstrapping its
    //      own search path right here instead of trusting a caller to
    //      have done it.
    DSBOX_MOD_RUNTIME_API BOOL WINAPI DSBoxRuntimeInitialize()
    {
        bool expected = false;
        auto& runtime = state();
        const bool isInitializer = runtime.started.compare_exchange_strong(expected, true);

        auto finishInitialization = [&runtime](bool succeeded)
        {
            {
                std::lock_guard<std::mutex> lock(runtime.initializationMutex);
                runtime.initializationSucceeded = succeeded;
                runtime.initializationComplete = true;
            }
            runtime.initializationChanged.notify_all();
        };

        if (!isInitializer)
        {
            std::unique_lock<std::mutex> lock(runtime.initializationMutex);
            runtime.initializationChanged.wait(lock, [&runtime]
            {
                return runtime.initializationComplete;
            });
            return runtime.initializationSucceeded ? TRUE : FALSE;
        }

        HMODULE module = nullptr;
        if (!::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&DSBoxRuntimeInitialize), &module))
        {
            finishInitialization(false);
            return FALSE;
        }

        try
        {
            // This DLL lives at fin-out/bin/framework/mod/ -- one level
            // UNDER the framework root (fin-out/bin/framework/ itself),
            // unlike an executable, which lives two levels under
            // fin-out/ (fin-out/bin/executables/<exe>) and therefore
            // needs "../framework". From here it's just "..". See
            // bootstrap_paths.hpp's own top comment for why this
            // distinction matters and why it silently broke here before
            // this fix.
            std::filesystem::path selfDir = ownModuleDirectory(module);
            dsbox::bootstrapFrameworkSearchPaths((selfDir / "..").string());
        }
        catch (const std::exception&)
        {
            // Can't even find our own path -- nothing more to do, but
            // don't let a thrown exception escape across the FFI/DLL
            // boundary into a Lua host.
            finishInitialization(false);
            return FALSE;
        }

        std::thread worker;
        try
        {
            worker = std::thread([module, finishInitialization]
            {
                bool succeeded = false;
                try
                {
                    succeeded = runtimeMain(module);
                }
                catch (...)
                {
                    // Do not allow an exception to escape the worker entry point.
                }
                finishInitialization(succeeded);
            });
        }

        catch (...)
        {
            finishInitialization(false);
            return FALSE;
        }

        runtime.worker = std::move(worker);

        std::unique_lock<std::mutex> lock(runtime.initializationMutex);
        runtime.initializationChanged.wait(lock, [&runtime]
        {
            return runtime.initializationComplete;
        });
        return runtime.initializationSucceeded ? TRUE : FALSE;
    }
}
