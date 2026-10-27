#pragma once

#include <dsbox/api.hpp>
#include <dsbox/plugin_export.hpp>
#include <dsbox/version_requirement.hpp>

#include <memory>
#include <string>

namespace dsbox
{
    class IModule;

    // Function signatures every dynamic module (.dll / .so) must export
    // with C linkage (see plugin_export.hpp for DSBOX_PLUGIN_EXPORT --
    // NOT dsbox::DSBOX_API, which is for DSBoxCore's own symbols):
    //
    //   #include <dsbox/plugin_export.hpp>
    //
    //   DSBOX_PLUGIN_EXPORT dsbox::IModule* DSBox_CreateModule();
    //   DSBOX_PLUGIN_EXPORT void            DSBox_DestroyModule(dsbox::IModule*);
    //
    // The destroy function is required (rather than `delete`-ing the
    // pointer from the host) because the module and the host may not
    // share a CRT/heap, especially on Windows.
    using ModuleCreateFn  = IModule* (*)();
    using ModuleDestroyFn = void (*)(IModule*);

    // Optional export: a plugin can declare which core version(s) it was
    // built against by exporting DSBox_GetRequiredCoreVersion. See
    // plugin_version.hpp for the DSBOX_REQUIRE_CORE_VERSION_* macros
    // that generate this for you -- most plugin authors should use those
    // rather than filling in this struct by hand.
    //
    // A plain C struct (not VersionRequirement itself) crosses the
    // plugin ABI boundary here deliberately: its layout must stay stable
    // regardless of how VersionRequirement's own internals evolve, since
    // an already-built plugin's binary can't be recompiled just because
    // the host's C++ class layout changed.
    struct DSBoxVersionRequirementC
    {
        int mode; // 0=any, 1=anyMinorPatch, 2=anyPatch, 3=exact, 4=range
        unsigned minMajor, minMinor, minPatch;
        unsigned maxMajor, maxMinor, maxPatch;
    };

    using ModuleGetRequiredVersionFn = void (*)(DSBoxVersionRequirementC*);

    // RAII wrapper around a platform shared library (LoadLibrary / dlopen).
    // Implementation lives in module_loader.cpp behind #ifdefs so no
    // platform headers leak into this public header.
    class DSBOX_API DynamicLibrary
    {
    public:
        DynamicLibrary() = default;
        ~DynamicLibrary();

        DynamicLibrary(const DynamicLibrary&) = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;

        DynamicLibrary(DynamicLibrary&& other) noexcept;
        DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;

        // Loads the shared library at `path`. Throws std::runtime_error
        // (with the platform-reported reason) on failure.
        static DynamicLibrary load(const std::string& path);

        // Resolves a symbol by name. Throws std::runtime_error if the
        // symbol cannot be found.
        void* getSymbol(const std::string& symbolName) const;

        // Like getSymbol(), but returns nullptr instead of throwing when
        // the symbol isn't present. Use this for genuinely optional
        // exports (e.g. DSBox_GetRequiredCoreVersion); reserve
        // getSymbol() for symbols the plugin ABI requires.
        void* tryGetSymbol(const std::string& symbolName) const noexcept;

        explicit operator bool() const noexcept
        {
            return m_handle != nullptr;
        }

        const std::string& path() const noexcept
        {
            return m_path;
        }

        void reset() noexcept;

    private:
        void* m_handle = nullptr;
        std::string m_path;
    };

    // A dynamically loaded module: the library that owns its code plus
    // the IModule instance created from it. Destroying this object stops
    // nothing by itself -- ModuleManager is responsible for calling
    // onStop/onShutdown before the DynamicModule (and therefore the
    // library) is destroyed, otherwise the module's vtable would be
    // unloaded while still in use.
    class LoadedDynamicModule
    {
    public:
        LoadedDynamicModule() = default;

        LoadedDynamicModule(
            DynamicLibrary library,
            IModule* instance,
            ModuleDestroyFn destroyFn)
            : m_library(std::move(library))
            , m_instance(instance)
            , m_destroyFn(destroyFn)
        {
        }

        LoadedDynamicModule(const LoadedDynamicModule&) = delete;
        LoadedDynamicModule& operator=(const LoadedDynamicModule&) = delete;

        LoadedDynamicModule(LoadedDynamicModule&& other) noexcept
        {
            *this = std::move(other);
        }

        LoadedDynamicModule& operator=(LoadedDynamicModule&& other) noexcept
        {
            if (this != &other)
            {
                destroyInstance();

                m_library = std::move(other.m_library);
                m_instance = other.m_instance;
                m_destroyFn = other.m_destroyFn;

                other.m_instance = nullptr;
                other.m_destroyFn = nullptr;
            }

            return *this;
        }

        ~LoadedDynamicModule()
        {
            destroyInstance();
        }

        IModule* instance() const noexcept
        {
            return m_instance;
        }

    private:
        void destroyInstance() noexcept
        {
            if (m_instance && m_destroyFn)
                m_destroyFn(m_instance);

            m_instance = nullptr;
            m_destroyFn = nullptr;
        }

        DynamicLibrary m_library;
        IModule* m_instance = nullptr;
        ModuleDestroyFn m_destroyFn = nullptr;
    };

    // Loads a module library from `path` and invokes its factory export.
    // Throws std::runtime_error on any failure (missing file, missing
    // symbols, factory returning nullptr, or a declared
    // DSBox_GetRequiredCoreVersion requirement that this host's version
    // does not satisfy -- checked before the factory is called at all).
    DSBOX_API LoadedDynamicModule loadDynamicModule(const std::string& path);
}
