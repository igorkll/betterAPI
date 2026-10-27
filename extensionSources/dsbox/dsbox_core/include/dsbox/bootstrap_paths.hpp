#pragma once

// dsbox::bootstrapFrameworkSearchPaths() -- call this as the VERY FIRST
// thing in main() (or, for DSBoxModRuntime, as the first thing in
// DSBoxRuntimeInitialize()), before touching any DSBox:: type, in any
// binary that implicitly links DSBox:: shared libraries (DSBoxUiDemo,
// DSBoxAdbTool, DSBoxLauncher, DSBoxModRuntime; see
// cmake/DSBoxDelayLoad.cmake).
//
// Why this is header-only, with no .cpp / no DSBoxCore dependency:
// -------------------------------------------------------------------------
// Since 793ebd1 (see docs/BUILD_LAYOUT.md), DSBox's shared libraries no
// longer all live next to the executable -- they're split across
// bin/framework/<subsystem>/ subfolders, and the executable's own
// implicit imports of them are deferred with /DELAYLOAD so this function
// gets a chance to widen the DLL search path before the first real call
// into any of them (see cmake/DSBoxDelayLoad.cmake for the full
// explanation of why only the direct dependencies need delay-loading).
//
// If this logic lived in DSBoxCore itself, calling it would require
// DSBoxCore.dll to already be loaded -- which is exactly the chicken-and-
// egg problem /DELAYLOAD is being used to avoid. So this stays a
// self-contained header, compiled directly into each executable's own
// binary, with no dependency on any DSBox DLL (not even DSBoxIni -- it
// reads the one config line it needs with a tiny hand-rolled scanner
// rather than linking IniDocument).
//
// Takes the FRAMEWORK ROOT directory (the folder that directly contains
// core/, scheduler/, ipc/, ... -- i.e. fin-out/bin/framework/), not the
// caller's own directory. The two are NOT the same thing for every
// caller: an executable lives at fin-out/bin/executables/<exe>, so its
// framework root is "../framework" relative to itself; DSBoxModRuntime
// lives at fin-out/bin/framework/mod/, so ITS framework root is just
// "..". Earlier versions of this function hardcoded "../framework/<x>"
// subfolder paths relative to the caller, which only worked for
// executables -- DSBoxModRuntime computing the same relative paths from
// its own directory landed on fin-out/bin/framework/framework/<x> (one
// "framework" too many) and silently failed to find DSBoxCore.dll
// whenever it was loaded without DSBoxModLoader's injection having
// separately configured the search path (e.g. ffi.load() straight from
// a game's embedded Lua/LuaJIT). Pushing the "where is the framework
// root from here" computation out to each caller (who already knows
// its own depth under fin-out/) removes that whole class of bug.
//
// What it does:
//  - Windows: calls SetDefaultDllDirectories() once (opts the process
//    into the "safe" DLL search order) and AddDllDirectory() for every
//    framework subfolder (resolved as frameworkRootDirectory / <name>),
//    plus any extra directories named by "[setup] additional_paths=" in
//    a config file, if one is given.
//    This single process-wide search path also covers plugins loaded
//    later via dsbox::loadDynamicModule() (ModuleLoader's own LoadLibrary
//    call resolves THAT plugin's implicit imports using the same
//    search path), so no separate plumbing is needed in ModuleLoader.
//  - Non-Windows (Linux/macOS): a no-op. ELF shared objects resolve their
//    dependencies via RPATH entries baked in at link time (see
//    CMAKE_BUILD_RPATH in the top-level CMakeLists.txt), which needs no
//    runtime step.
//
// additionalPathsConfigFile, if non-empty, is read relative to nothing in
// particular -- pass an absolute path (e.g. "<exeDir>/config/setup.ini").
// A missing file, or one with no [setup] section, is not an error: the
// fixed framework subfolders below are always added regardless.

#include <string>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <fstream>
    #include <sstream>
#endif

namespace dsbox
{
    namespace detail
    {
        // The fixed set of framework subfolders, relative to the
        // FRAMEWORK ROOT (fin-out/bin/framework/, not the caller's own
        // directory -- see this header's top comment), as laid out by
        // the dsbox_set_output_subdir() calls in the top-level
        // CMakeLists.txt. Kept here (not derived automatically) because
        // it mirrors a mapping that only changes when a module is added
        // or moved -- see docs/BUILD_LAYOUT.md for the authoritative list.
        inline const std::vector<std::string>& defaultFrameworkSubfolders()
        {
            static const std::vector<std::string> kSubfolders = {
                "core",
                "scheduler",
                "ipc",
                "io",
                "android",
                "cli",
                "net",
                "net/tls",
                "ui",
                "ui/DSBoxSDL",
                "render",
                "mod",
            };
            return kSubfolders;
        }

#if defined(_WIN32)
        // Minimal, dependency-free scan for exactly one key: splits on
        // ';' or ',' into the returned vector. Not a general INI parser
        // (deliberately -- see the file header comment).
        inline std::vector<std::string> readAdditionalPaths(const std::string& configFile)
        {
            std::vector<std::string> result;

            std::ifstream in(configFile);
            if (!in)
                return result;

            std::string line;
            bool inSetupSection = false;

            while (std::getline(in, line))
            {
                // Trim leading whitespace.
                std::size_t start = line.find_first_not_of(" \t\r\n");
                if (start == std::string::npos)
                    continue;
                line = line.substr(start);

                if (line.empty() || line[0] == ';' || line[0] == '#')
                    continue;

                if (line.front() == '[')
                {
                    inSetupSection = (line.rfind("[setup]", 0) == 0);
                    continue;
                }

                if (!inSetupSection)
                    continue;

                if (line.rfind("additional_paths", 0) != 0)
                    continue;

                std::size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                std::string value = line.substr(eq + 1);

                std::stringstream valueStream(value);
                std::string token;
                while (std::getline(valueStream, token, ';'))
                {
                    std::size_t tStart = token.find_first_not_of(" \t\r\n");
                    std::size_t tEnd = token.find_last_not_of(" \t\r\n");
                    if (tStart == std::string::npos)
                        continue;
                    result.push_back(token.substr(tStart, tEnd - tStart + 1));
                }

                break; // only one additional_paths= line is honored
            }

            return result;
        }
#endif
    }

    inline void bootstrapFrameworkSearchPaths(
        const std::string& frameworkRootDirectory,
        const std::string& additionalPathsConfigFile = std::string())
    {
#if defined(_WIN32)
        // Opt the process into the search order where AddDllDirectory()
        // entries are actually consulted by LoadLibrary (implicit or
        // explicit, delay-loaded or not). Safe to call once at startup.
        ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

        auto addDir = [&](const std::string& relativeOrAbsolute)
        {
            std::string full = relativeOrAbsolute;

            bool isAbsolute =
                full.size() >= 2 && (full[1] == ':' || (full[0] == '\\' && full[1] == '\\'));

            if (!isAbsolute)
                full = frameworkRootDirectory + "\\" + relativeOrAbsolute;

            // Widen char -> wchar_t the simple way; these are filesystem
            // paths built from our own config/CMake layout, not arbitrary
            // user text, so a naive conversion is sufficient here.
            std::wstring wide(full.begin(), full.end());
            ::AddDllDirectory(wide.c_str());
        };

        for (const auto& subfolder : detail::defaultFrameworkSubfolders())
            addDir(subfolder);

        if (!additionalPathsConfigFile.empty())
        {
            for (const auto& extra : detail::readAdditionalPaths(additionalPathsConfigFile))
                addDir(extra);
        }
#else
        // Linux/macOS: RPATH (baked in at link time, see
        // CMAKE_BUILD_RPATH in the top-level CMakeLists.txt) already
        // resolves every framework subfolder with no runtime step.
        // Parameters intentionally unused here.
        (void)frameworkRootDirectory;
        (void)additionalPathsConfigFile;
#endif
    }

    // Shared libraries that contribute DSBOX_STATIC_MODULE registrations
    // must be loaded before ModuleManager::registerStaticModules() runs.
    // Executables delay-load these DLLs so the framework search path can be
    // installed first, which otherwise means their static initializers have
    // not run at registration time.
    inline void preloadFrameworkModule(const char* dllName)
    {
#if defined(_WIN32)
        if (::LoadLibraryA(dllName) == nullptr)
            throw std::runtime_error(
                std::string("Failed to preload framework module '") + dllName +
                "' (Windows error " + std::to_string(::GetLastError()) + ")");
#else
        (void)dllName;
#endif
    }
}
