#pragma once

#include <dsbox/communication_manager.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace dsbox
{
    class ModuleManager;

    // Context is passed to every module during its lifecycle callbacks.
    // It exposes the shared communication bus and a small typed service
    // locator so modules can publish/consume cross-cutting services
    // (logger, config, etc.) without depending on each other directly.
    //
    // Thread safety: comm() itself is thread-safe (see
    // CommunicationManager). provide()/get()/has() are safe to call
    // concurrently from multiple threads; the service map is protected by
    // an internal mutex that is never held during a caller's own code,
    // only while touching the map itself. provide() replaces any
    // previously provided service of the same type -- a shared_ptr
    // already returned by an earlier get() call remains valid regardless
    // (ordinary shared_ptr reference counting), so replacing a service
    // while someone else is still holding the old one is safe.
    class Context
    {
    public:
        Context() = default;

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        Context(Context&&) = delete;
        Context& operator=(Context&&) = delete;

        CommunicationManager& comm() noexcept
        {
            return m_comm;
        }

        const CommunicationManager& comm() const noexcept
        {
            return m_comm;
        }

        void bindModuleManager(ModuleManager* moduleManager) noexcept
        {
            m_moduleManager.store(moduleManager, std::memory_order_release);
        }

        ModuleManager* modules() const noexcept
        {
            return m_moduleManager.load(std::memory_order_acquire);
        }

        /*
         * Service locator
         */

        template<typename T>
        void provide(std::shared_ptr<T> service)
        {
            std::unique_lock<std::shared_mutex> lock(m_servicesMutex);

            m_services[std::type_index(typeid(T))] =
                std::static_pointer_cast<void>(std::move(service));
        }

        template<typename T>
        std::shared_ptr<T> get() const
        {
            std::shared_lock<std::shared_mutex> lock(m_servicesMutex);

            auto it = m_services.find(std::type_index(typeid(T)));

            if (it == m_services.end())
                return nullptr;

            return std::static_pointer_cast<T>(it->second);
        }

        template<typename T>
        bool has() const noexcept
        {
            std::shared_lock<std::shared_mutex> lock(m_servicesMutex);
            return m_services.contains(std::type_index(typeid(T)));
        }

    private:
        CommunicationManager m_comm;

        mutable std::shared_mutex m_servicesMutex;
        std::unordered_map<std::type_index, std::shared_ptr<void>> m_services;

        std::atomic<ModuleManager*> m_moduleManager{ nullptr };
    };
}
