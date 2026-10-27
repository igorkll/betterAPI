#pragma once

#include <dsbox/api.hpp>
#include <dsbox/context.hpp>
#include <dsbox/module.hpp>
#include <dsbox/module_loader.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dsbox
{
    enum class ModuleState
    {
        Registered, // known to the manager, callbacks not yet invoked
        Initialized, // onInit done
        Started, // onStart done, fully operational
        Stopped, // onStop done, onShutdown not yet called
        ShutDown, // onShutdown done, module is inert
    };

    // Thrown for any registration/resolution problem: duplicate names,
    // missing dependencies, dependency cycles, invalid state transitions.
    class DSBOX_API ModuleError : public std::runtime_error
    {
    public:
        explicit ModuleError(const std::string& message)
            : std::runtime_error(message)
        {
        }
    };

    // What to do when a module's onInit/onStart throws during startAll().
    enum class ModuleErrorAction
    {
        // Rethrow immediately, aborting startAll(). This is the default
        // when no error handler is supplied, matching the historical
        // (pre-error-handler) behavior exactly.
        Abort,

        // Mark this module -- and every module that transitively depends
        // on it -- as skipped, and continue starting the rest. Skipped
        // modules stay in the Registered state and never receive any
        // lifecycle callback; a module whose onInit already succeeded
        // before onStart failed gets onShutdown called once, to mirror
        // the onInit it already ran.
        Skip,
    };

    // Invoked with the failing module's name and the exception it threw.
    // Return Abort (default behavior, propagate and stop) or Skip
    // (isolate the failure and keep starting unrelated modules).
    using ModuleErrorHandler =
        std::function<ModuleErrorAction(std::string_view moduleName, const std::exception& error)>;

    // Owns every module known to the runtime -- whether it was linked
    // statically into the binary or loaded from a shared library -- and
    // drives their lifecycle in dependency order.
    //
    // Thread safety: all public methods are safe to call concurrently
    // from multiple threads with respect to ModuleManager's own internal
    // state. The registry (registerModule/loadDynamic/has/find/stateOf/
    // moduleNames/unload's bookkeeping) is protected by an internal
    // mutex that is never held while an IModule callback (onInit/onStart/
    // onStop/onShutdown) is executing, so a module is free to call back
    // into the same ModuleManager (e.g. find()) from within its own
    // lifecycle callback without deadlocking. Concurrently unloading a
    // module while one of its lifecycle callbacks is in flight on
    // another thread is not supported.
    //
    // Typical usage:
    //
    //   Context context;
    //   ModuleManager modules(context);
    //
    //   modules.registerStaticModules();          // self-registered statics
    //   modules.loadDynamic("plugins/foo.dll");    // optional, runtime plugins
    //
    //   modules.startAll();
    //   ...
    //   modules.stopAll();
    class DSBOX_API ModuleManager
    {
    public:
        explicit ModuleManager(Context& context)
            : m_context(context)
        {
            m_context.bindModuleManager(this);
        }

        ~ModuleManager();

        ModuleManager(const ModuleManager&) = delete;
        ModuleManager& operator=(const ModuleManager&) = delete;

        /*
         * Registration
         */

        // Registers a statically/host-owned module instance. The manager
        // does not take ownership of module lifetime beyond storing the
        // pointer; `owned` (if provided) keeps it alive.
        void registerModule(std::unique_ptr<IModule> owned);

        // Loads a shared library from `path` and registers the module it
        // exports. Throws ModuleError (wrapping the loader's message) on
        // failure. The library stays loaded for as long as the module is
        // registered.
        IModule& loadDynamic(const std::string& path);

        // Registers every module that used DSBOX_STATIC_MODULE in this
        // binary (see static_registry.hpp). Safe to call multiple times;
        // already-registered names are skipped.
        void registerStaticModules();

        /*
         * Lookup
         */

        bool has(std::string_view name) const noexcept;
        IModule* find(std::string_view name) const noexcept;
        ModuleState stateOf(std::string_view name) const;

        std::vector<std::string> moduleNames() const;

        /*
         * Lifecycle - bulk
         */

        // Computes a dependency-respecting order over every registered
        // module and runs onInit (full pass), then onStart (full pass).
        // Throws ModuleError if a dependency is missing or a cycle is
        // detected; no callbacks are invoked in that case. The resolved
        // order is recorded for stopAll() before any callback runs, so
        // cleanup always follows correct reverse-dependency order even
        // if a module fails partway through.
        //
        // `errorHandler`, if supplied, is consulted whenever a module's
        // onInit/onStart throws. Returning ModuleErrorAction::Skip
        // isolates that module (and everything depending on it) and lets
        // startAll() continue with the rest instead of aborting. Leave
        // it null (the default) to preserve the original all-or-nothing
        // behavior: the first failure propagates out of startAll().
        void startAll(const ModuleErrorHandler& errorHandler = nullptr);

        // Runs onStop then onShutdown over every module currently at or
        // beyond Started, in reverse dependency order. Safe to call even
        // if startAll() only got partway through.
        void stopAll() noexcept;

        /*
         * Lifecycle - single module
         */

        // Stops+shuts down a single module. Throws ModuleError if other
        // running modules still depend on it (reload/unload them first).
        void unload(std::string_view name);

        // Re-runs onStop/onShutdown followed by onInit/onStart on the
        // SAME module instance -- a state reset, not a hot code reload.
        // This does not reread the module's code: for a statically
        // linked module it re-executes the same compiled callbacks: for
        // a dynamically loaded one it re-invokes them on the already
        // -loaded library. Useful for recovering a module from a bad
        // internal state without restarting the whole host. To actually
        // pick up new code for a dynamically loaded module, unload() it
        // and loadDynamic() the (rebuilt) library again instead.
        void reload(std::string_view name);

    private:
        struct Entry
        {
            IModule* instance = nullptr;
            ModuleState state = ModuleState::Registered;

            // Exactly one of these is engaged, depending on where the
            // module came from; both may be empty for externally-owned
            // raw pointers registered via internal helpers.
            std::unique_ptr<IModule> owned;
            std::unique_ptr<LoadedDynamicModule> dynamic;
        };

        std::vector<std::string> resolveOrder() const;
        void initAndStart(const std::vector<std::string>& order, const ModuleErrorHandler& errorHandler);
        void stopAndShutdown(const std::vector<std::string>& order) noexcept;
        void assertNoDependents(std::string_view name) const;
        void skipTransitively(const std::string& failedName, std::unordered_set<std::string>& skipped) const;

        IModule* instanceOf(const std::string& name);
        void setState(const std::string& name, ModuleState state);
        ModuleState getState(const std::string& name);

        mutable std::mutex m_mutex;

        Context& m_context;
        std::unordered_map<std::string, Entry> m_modules;

        // Order used the last time startAll() was attempted; reused by
        // stopAll() so shutdown mirrors startup even if new modules were
        // registered afterwards without a restart, and even if startAll()
        // itself did not fully succeed.
        std::vector<std::string> m_startOrder;
    };
}
