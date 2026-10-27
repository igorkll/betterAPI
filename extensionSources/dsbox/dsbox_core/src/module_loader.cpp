#include <dsbox/module_loader.hpp>
#include <dsbox/module.hpp>
#include <dsbox/version.hpp>

#include <stdexcept>
#include <utility>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace dsbox
{
    namespace
    {
        [[noreturn]] void throwLoadError(const std::string& path)
        {
#if defined(_WIN32)
            DWORD errorCode = ::GetLastError();
            throw std::runtime_error(
                "Failed to load module '" + path + "' (error " + std::to_string(errorCode) + ")");
#else
            const char* reason = dlerror();
            throw std::runtime_error(
                "Failed to load module '" + path + "': " + (reason ? reason : "unknown error"));
#endif
        }

        [[noreturn]] void throwSymbolError(const std::string& path, const std::string& symbol)
        {
#if defined(_WIN32)
            DWORD errorCode = ::GetLastError();
            throw std::runtime_error(
                "Symbol '" + symbol + "' not found in module '" + path +
                "' (error " + std::to_string(errorCode) + ")");
#else
            const char* reason = dlerror();
            throw std::runtime_error(
                "Symbol '" + symbol + "' not found in module '" + path + "': " +
                (reason ? reason : "unknown error"));
#endif
        }
    }

    DynamicLibrary::~DynamicLibrary()
    {
        reset();
    }

    DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
        : m_handle(other.m_handle)
        , m_path(std::move(other.m_path))
    {
        other.m_handle = nullptr;
    }

    DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
    {
        if (this != &other)
        {
            reset();

            m_handle = other.m_handle;
            m_path = std::move(other.m_path);

            other.m_handle = nullptr;
        }

        return *this;
    }

    void DynamicLibrary::reset() noexcept
    {
        if (!m_handle)
            return;

#if defined(_WIN32)
        ::FreeLibrary(static_cast<HMODULE>(m_handle));
#else
        ::dlclose(m_handle);
#endif

        m_handle = nullptr;
    }

    DynamicLibrary DynamicLibrary::load(const std::string& path)
    {
        DynamicLibrary library;

#if defined(_WIN32)
        HMODULE handle = ::LoadLibraryA(path.c_str());
#else
        void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

        if (!handle)
            throwLoadError(path);

        library.m_handle = handle;
        library.m_path = path;

        return library;
    }

    void* DynamicLibrary::tryGetSymbol(const std::string& symbolName) const noexcept
    {
        if (!m_handle)
            return nullptr;

#if defined(_WIN32)
        return reinterpret_cast<void*>(
            ::GetProcAddress(static_cast<HMODULE>(m_handle), symbolName.c_str()));
#else
        // Clear any pending error before the lookup, since dlsym() can
        // legitimately return nullptr for a symbol whose value is 0.
        ::dlerror();
        void* symbol = ::dlsym(m_handle, symbolName.c_str());
        const char* error = ::dlerror();
        return error ? nullptr : symbol;
#endif
    }

    void* DynamicLibrary::getSymbol(const std::string& symbolName) const
    {
        if (!m_handle)
            throw std::runtime_error("getSymbol() called on an unloaded library");

        void* symbol = tryGetSymbol(symbolName);

        if (!symbol)
            throwSymbolError(m_path, symbolName);

        return symbol;
    }

    namespace
    {
        VersionRequirement versionRequirementFromC(const DSBoxVersionRequirementC& c)
        {
            switch (c.mode)
            {
                case 1:
                    return VersionRequirement::anyMinorPatch(c.minMajor);
                case 2:
                    return VersionRequirement::anyPatch(c.minMajor, c.minMinor);
                case 3:
                    return VersionRequirement::exact(Version{ c.minMajor, c.minMinor, c.minPatch });
                case 4:
                    return VersionRequirement::range(
                        Version{ c.minMajor, c.minMinor, c.minPatch },
                        Version{ c.maxMajor, c.maxMinor, c.maxPatch });
                default:
                    return VersionRequirement::any();
            }
        }
    }

    LoadedDynamicModule loadDynamicModule(const std::string& path)
    {
        DynamicLibrary library = DynamicLibrary::load(path);

        // Optional: if the plugin declares a required core version range
        // via DSBox_GetRequiredCoreVersion, check it before calling
        // DSBox_CreateModule() at all -- that's where an actual ABI
        // mismatch would be most likely to misbehave. A plugin that
        // doesn't export this symbol is treated as compatible with any
        // core version, so existing plugins keep loading unchanged.
        if (void* versionSym = library.tryGetSymbol("DSBox_GetRequiredCoreVersion"))
        {
            auto getRequiredVersion = reinterpret_cast<ModuleGetRequiredVersionFn>(versionSym);

            DSBoxVersionRequirementC requirementC{};
            getRequiredVersion(&requirementC);

            VersionRequirement requirement = versionRequirementFromC(requirementC);
            Version current{ versionMajor(), versionMinor(), versionPatch() };

            if (!requirement.isSatisfiedBy(current))
                throw std::runtime_error(
                    "Module '" + path + "' requires DSBox core " + requirement.describe() +
                    ", but this host is " + current.toString());
        }

        auto createFn = reinterpret_cast<ModuleCreateFn>(
            library.getSymbol("DSBox_CreateModule"));
        auto destroyFn = reinterpret_cast<ModuleDestroyFn>(
            library.getSymbol("DSBox_DestroyModule"));

        IModule* instance = createFn();

        if (!instance)
            throw std::runtime_error(
                "Module '" + path + "' factory DSBox_CreateModule() returned nullptr");

        return LoadedDynamicModule(std::move(library), instance, destroyFn);
    }
}
