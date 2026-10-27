#pragma once

#include <dsbox_config/api.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace dsbox
{
    class ModuleManager;
}

namespace dsbox_config
{
    // Outcome of trying to load one "<alias>.path" entry from a module
    // config. Plain data -- kept header-only/inline so it can cross the
    // DSBoxConfig.dll boundary without needing its own export macro.
    struct ModuleLoadResult
    {
        std::string alias;      // grouping key used in the config file
        std::string path;       // path as resolved (baseDir applied)
        bool enabled = true;    // false if "<alias>.enabled" was false
        bool loaded = false;    // true if ModuleManager::loadDynamic() succeeded
        std::string moduleName; // the module's own IModule::name(), once loaded
        std::string error;      // populated when enabled && !loaded
    };

    struct ModuleLoadReport
    {
        std::vector<ModuleLoadResult> results;

        // True if the config parsed and every enabled entry loaded.
        // Disabled entries are never counted as failures.
        bool allSucceeded() const
        {
            for (const auto& result : results)
                if (result.enabled && !result.loaded)
                    return false;

            return true;
        }

        // Enabled entries that failed to load, plus a single entry with
        // alias "<ini>" if the config text itself was malformed.
        std::vector<ModuleLoadResult> failures() const
        {
            std::vector<ModuleLoadResult> out;

            for (const auto& result : results)
                if (result.enabled && !result.loaded)
                    out.push_back(result);

            return out;
        }
    };

    // Parses `iniText` and loads every enabled module it declares into
    // `modules`. The host is responsible for obtaining `iniText` (reading
    // a file, fetching it over the network, etc.) -- this class only
    // interprets it; it never touches the filesystem itself.
    //
    // Expected format:
    //
    //   [modules]
    //   console.path      = plugins/dsbox_dsx.dll
    //   telemetry.path    = plugins/telemetry.so
    //   telemetry.enabled = false
    //
    // Every "<alias>.path" key found under [modules] describes one
    // module to load. The optional "<alias>.enabled" key (default true)
    // lets an entry be turned off without deleting it. `alias` only
    // groups the two keys together in the file -- the module's real
    // identity, as seen by ModuleManager, is whatever its own
    // IModule::name() returns.
    //
    // Individual module failures (bad path, missing exports, duplicate
    // name, ...) are collected into the returned report rather than
    // thrown, so one broken entry can't prevent the rest from loading.
    // A malformed INI document is reported the same way, as a single
    // failure entry -- check ModuleLoadReport::allSucceeded() /
    // failures() rather than wrapping this call in try/catch.
    //
    // Relative paths are resolved against `baseDir` when it is
    // non-empty (typically the directory containing the config file);
    // absolute paths are used as-is.
    class DSBOX_CONFIG_API ModuleConfigLoader
    {
    public:
        static ModuleLoadReport loadFromIni(
            dsbox::ModuleManager& modules,
            std::string_view iniText,
            std::string_view baseDir = {});
    };
}
