#include <dsbox/static_registry.hpp>

#include <mutex>

namespace dsbox
{
    namespace
    {
        std::mutex& registryMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
    }

    StaticModuleRegistry& StaticModuleRegistry::instance()
    {
        // Factories may have their callable manager/destructor code in
        // another DLL. Windows can unload that DLL before DSBoxCore's
        // CRT teardown destroys this registry, leaving std::function's
        // destructor pointing into unmapped code. Keep this process-wide
        // registry alive until the OS reclaims process memory instead of
        // running a cross-DLL static-destruction sequence.
        static StaticModuleRegistry* registry = new StaticModuleRegistry();
        return *registry;
    }

    bool StaticModuleRegistry::add(ModuleFactory factory)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        m_factories.push_back(std::move(factory));
        return true;
    }

    std::vector<ModuleFactory> StaticModuleRegistry::factories() const
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        return m_factories;
    }
}
