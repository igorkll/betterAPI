#include <dsbox/module_manager.hpp>
#include <dsbox/static_registry.hpp>

#include <algorithm>
#include <deque>
#include <sstream>

namespace dsbox
{
    namespace
    {
        std::string joinNames(const std::vector<std::string>& names)
        {
            std::ostringstream out;

            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (i != 0)
                    out << " -> ";

                out << names[i];
            }

            return out.str();
        }
    }

    ModuleManager::~ModuleManager()
    {
        stopAll();
    }

    /*
     * Small internal helpers: each takes the lock for just long enough to
     * read or write m_modules, and never while an IModule callback is
     * executing. This is what lets a module safely call back into this
     * same ModuleManager (e.g. find()) from within its own onInit/onStart
     * without deadlocking.
     */

    IModule* ModuleManager::instanceOf(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_modules.find(name);
        return it == m_modules.end() ? nullptr : it->second.instance;
    }

    void ModuleManager::setState(const std::string& name, ModuleState state)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_modules.find(name);
        if (it != m_modules.end())
            it->second.state = state;
    }

    ModuleState ModuleManager::getState(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_modules.find(name);
        return it == m_modules.end() ? ModuleState::Registered : it->second.state;
    }

    void ModuleManager::registerModule(std::unique_ptr<IModule> owned)
    {
        if (!owned)
            throw ModuleError("registerModule() called with a null module");

        std::string name(owned->name());

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_modules.contains(name))
            throw ModuleError("A module named '" + name + "' is already registered");

        Entry entry;
        entry.instance = owned.get();
        entry.owned = std::move(owned);

        m_modules.emplace(std::move(name), std::move(entry));
    }

    IModule& ModuleManager::loadDynamic(const std::string& path)
    {
        LoadedDynamicModule loaded = loadDynamicModule(path);
        IModule* instance = loaded.instance();

        std::string name(instance->name());

        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_modules.contains(name))
            throw ModuleError(
                "A module named '" + name + "' is already registered (loading '" + path + "')");

        Entry entry;
        entry.instance = instance;
        entry.dynamic = std::make_unique<LoadedDynamicModule>(std::move(loaded));

        m_modules.emplace(std::move(name), std::move(entry));

        return *instance;
    }

    void ModuleManager::registerStaticModules()
    {
        for (const auto& factory : StaticModuleRegistry::instance().factories())
        {
            auto instance = factory();

            try
            {
                registerModule(std::move(instance));
            }
            catch (const ModuleError&)
            {
                // Already registered -- e.g. a second call to
                // registerStaticModules(), possibly racing with another
                // thread. That's fine, this call is idempotent by
                // contract.
            }
        }
    }

    bool ModuleManager::has(std::string_view name) const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_modules.contains(std::string(name));
    }

    IModule* ModuleManager::find(std::string_view name) const noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_modules.find(std::string(name));

        if (it == m_modules.end())
            return nullptr;

        return it->second.instance;
    }

    ModuleState ModuleManager::stateOf(std::string_view name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_modules.find(std::string(name));

        if (it == m_modules.end())
            throw ModuleError("No module named '" + std::string(name) + "' is registered");

        return it->second.state;
    }

    std::vector<std::string> ModuleManager::moduleNames() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<std::string> names;
        names.reserve(m_modules.size());

        for (const auto& [name, entry] : m_modules)
            names.push_back(name);

        return names;
    }

    std::vector<std::string> ModuleManager::resolveOrder() const
    {
        // Kahn's algorithm over the dependency graph, with a DFS-based
        // cycle report if it gets stuck so the error names the offending
        // chain instead of just "there is a cycle somewhere".
        //
        // Held under the lock for its whole duration: this only reads
        // m_modules and calls each module's dependencies(), which is
        // documented (IModule) to be a cheap, side-effect-free query, not
        // a place to do real work -- so this is a short critical section
        // in practice, not a place where holding the lock risks blocking
        // other threads for long.

        std::lock_guard<std::mutex> lock(m_mutex);

        std::unordered_map<std::string, int> inDegree;
        std::unordered_map<std::string, std::vector<std::string>> dependents;

        for (const auto& [name, entry] : m_modules)
            inDegree.emplace(name, 0);

        for (const auto& [name, entry] : m_modules)
        {
            for (const auto& dep : entry.instance->dependencies())
            {
                if (!m_modules.contains(dep))
                    throw ModuleError(
                        "Module '" + name + "' depends on unknown module '" + dep + "'");

                dependents[dep].push_back(name);
                inDegree[name] += 1;
            }
        }

        std::deque<std::string> ready;
        for (const auto& [name, degree] : inDegree)
            if (degree == 0)
                ready.push_back(name);

        // Deterministic ordering among independent modules.
        std::sort(ready.begin(), ready.end());

        std::vector<std::string> order;
        order.reserve(m_modules.size());

        while (!ready.empty())
        {
            std::string name = ready.front();
            ready.pop_front();
            order.push_back(name);

            auto it = dependents.find(name);
            if (it == dependents.end())
                continue;

            std::vector<std::string> newlyReady;

            for (const auto& dependent : it->second)
            {
                if (--inDegree[dependent] == 0)
                    newlyReady.push_back(dependent);
            }

            std::sort(newlyReady.begin(), newlyReady.end());
            for (auto& n : newlyReady)
                ready.push_back(std::move(n));
        }

        if (order.size() != m_modules.size())
        {
            // Something is left with inDegree > 0: find one such node and
            // walk its dependency chain until we revisit a node, to report
            // a concrete cycle.
            std::vector<std::string> cycle;
            std::unordered_set<std::string> visited;

            std::string current;
            for (const auto& [name, degree] : inDegree)
            {
                if (degree > 0)
                {
                    current = name;
                    break;
                }
            }

            while (visited.insert(current).second)
            {
                cycle.push_back(current);
                const auto& deps = m_modules.at(current).instance->dependencies();

                bool advanced = false;
                for (const auto& dep : deps)
                {
                    if (inDegree.at(dep) > 0)
                    {
                        current = dep;
                        advanced = true;
                        break;
                    }
                }

                if (!advanced)
                    break;
            }

            cycle.push_back(current);

            throw ModuleError("Dependency cycle detected: " + joinNames(cycle));
        }

        return order;
    }

    void ModuleManager::skipTransitively(
        const std::string& failedName,
        std::unordered_set<std::string>& skipped) const
    {
        skipped.insert(failedName);

        std::lock_guard<std::mutex> lock(m_mutex);

        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const auto& [name, entry] : m_modules)
            {
                if (skipped.contains(name))
                    continue;

                for (const auto& dep : entry.instance->dependencies())
                {
                    if (skipped.contains(dep))
                    {
                        skipped.insert(name);
                        changed = true;
                        break;
                    }
                }
            }
        }
    }

    void ModuleManager::initAndStart(
        const std::vector<std::string>& order,
        const ModuleErrorHandler& errorHandler)
    {
        std::unordered_set<std::string> skipped;

        // Phase 1: onInit across every non-skipped module, in dependency
        // order.
        for (const auto& name : order)
        {
            if (skipped.contains(name))
                continue;

            IModule* instance = instanceOf(name);
            if (!instance)
                continue;

            try
            {
                instance->onInit(m_context);
                setState(name, ModuleState::Initialized);
            }
            catch (const std::exception& error)
            {
                if (!errorHandler || errorHandler(name, error) != ModuleErrorAction::Skip)
                    throw;

                skipTransitively(name, skipped);
            }
        }

        // Phase 2: onStart, only for modules that made it through onInit.
        // Both phases finishing in full before the next begins mirrors
        // the two-phase stop/shutdown in stopAndShutdown().
        for (const auto& name : order)
        {
            if (skipped.contains(name))
                continue;

            if (getState(name) != ModuleState::Initialized)
                continue;

            IModule* instance = instanceOf(name);
            if (!instance)
                continue;

            try
            {
                instance->onStart(m_context);
                setState(name, ModuleState::Started);
            }
            catch (const std::exception& error)
            {
                if (!errorHandler || errorHandler(name, error) != ModuleErrorAction::Skip)
                    throw;

                // onInit already succeeded for this module -- give it a
                // matching onShutdown so it isn't left half-initialized.
                // A second failure here must not hide the one already
                // reported to errorHandler.
                try
                {
                    instance->onShutdown(m_context);
                }
                catch (...)
                {
                }
                setState(name, ModuleState::ShutDown);

                skipTransitively(name, skipped);
            }
        }
    }

    void ModuleManager::startAll(const ModuleErrorHandler& errorHandler)
    {
        std::vector<std::string> order = resolveOrder();

        // Recorded before any callback runs: stopAll() then always has
        // the correct reverse-dependency order for cleanup, even if a
        // module fails (Abort path) partway through initAndStart().
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_startOrder = order;
        }

        initAndStart(order, errorHandler);
    }

    void ModuleManager::stopAndShutdown(const std::vector<std::string>& order) noexcept
    {
        // Two full passes, mirroring initAndStart(): every module finishes
        // onStop (in reverse dependency order) before any module's
        // onShutdown runs. Without this, a dependency could be fully torn
        // down (ShutDown) while a dependent's onStop is still executing.
        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            if (getState(*it) != ModuleState::Started)
                continue;

            IModule* instance = instanceOf(*it);
            if (!instance)
                continue;

            instance->onStop(m_context);
            setState(*it, ModuleState::Stopped);
        }

        for (auto it = order.rbegin(); it != order.rend(); ++it)
        {
            // Stopped: the normal case, onStop already ran above.
            // Initialized: onInit succeeded but onStart was never
            // attempted for this module -- e.g. startAll() aborted
            // (Abort, the default) because a LATER module in the order
            // failed its own onInit, before the onStart phase for
            // anyone had begun. Either way, onInit having succeeded
            // means onShutdown is owed to release whatever it acquired,
            // even though onStop never applies here.
            ModuleState state = getState(*it);
            if (state != ModuleState::Stopped && state != ModuleState::Initialized)
                continue;

            IModule* instance = instanceOf(*it);
            if (!instance)
                continue;

            instance->onShutdown(m_context);
            setState(*it, ModuleState::ShutDown);
        }
    }

    void ModuleManager::stopAll() noexcept
    {
        std::vector<std::string> order;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            order = m_startOrder;

            if (order.empty())
            {
                // startAll() was never called (or hadn't reached the
                // point of recording an order): fall back to shutting
                // down whatever did make it past onInit, in an
                // arbitrary-but-stable order.
                order.reserve(m_modules.size());
                for (const auto& [name, entry] : m_modules)
                    order.push_back(name);
                std::sort(order.begin(), order.end());
            }
        }

        stopAndShutdown(order);

        std::lock_guard<std::mutex> lock(m_mutex);
        m_startOrder.clear();
    }

    void ModuleManager::assertNoDependents(std::string_view name) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        for (const auto& [otherName, entry] : m_modules)
        {
            if (otherName == name)
                continue;

            if (entry.state != ModuleState::Started && entry.state != ModuleState::Initialized)
                continue;

            for (const auto& dep : entry.instance->dependencies())
            {
                if (dep == name)
                    throw ModuleError(
                        "Cannot unload '" + std::string(name) + "': module '" + otherName +
                        "' still depends on it");
            }
        }
    }

    void ModuleManager::unload(std::string_view name)
    {
        std::string nameStr(name);

        if (!has(nameStr))
            throw ModuleError("No module named '" + nameStr + "' is registered");

        assertNoDependents(name);

        IModule* instance = instanceOf(nameStr);
        ModuleState state = getState(nameStr);

        if (state == ModuleState::Started)
        {
            instance->onStop(m_context);
            setState(nameStr, ModuleState::Stopped);
            state = ModuleState::Stopped;
        }

        if (state == ModuleState::Stopped)
        {
            instance->onShutdown(m_context);
            setState(nameStr, ModuleState::ShutDown);
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        m_modules.erase(nameStr);

        auto orderIt = std::find(m_startOrder.begin(), m_startOrder.end(), nameStr);
        if (orderIt != m_startOrder.end())
            m_startOrder.erase(orderIt);
    }

    void ModuleManager::reload(std::string_view name)
    {
        std::string nameStr(name);

        if (!has(nameStr))
            throw ModuleError("No module named '" + nameStr + "' is registered");

        assertNoDependents(name);

        IModule* instance = instanceOf(nameStr);
        ModuleState state = getState(nameStr);

        if (state == ModuleState::Started)
        {
            instance->onStop(m_context);
            setState(nameStr, ModuleState::Stopped);
            state = ModuleState::Stopped;
        }

        if (state == ModuleState::Stopped)
        {
            instance->onShutdown(m_context);
            setState(nameStr, ModuleState::ShutDown);
        }

        instance->onInit(m_context);
        setState(nameStr, ModuleState::Initialized);

        instance->onStart(m_context);
        setState(nameStr, ModuleState::Started);
    }
}
