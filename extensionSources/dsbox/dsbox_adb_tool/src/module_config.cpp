#include "module_config.hpp"

#include <dsbox/module_manager.hpp>
#include <dsbox_config/module_config_loader.hpp>

#include "launch_options.hpp"

#include <cstdio>

namespace dsbox_adb_tool
{
    bool loadModuleConfig(
        dsbox::ModuleManager& modules,
        const std::filesystem::path& path,
        const std::filesystem::path& moduleBaseDir)
    {
        std::string iniText;

        if (!readFile(path, iniText))
        {
            std::fprintf(stderr, "Failed to read config '%s'\n", path.string().c_str());
            return false;
        }

        auto report = dsbox_config::ModuleConfigLoader::loadFromIni(
            modules, iniText, moduleBaseDir.string());

        for (const auto& result : report.results)
        {
            if (!result.enabled)
            {
                std::printf("[config] '%s' skipped (disabled)\n", result.alias.c_str());
            }
            else if (result.loaded)
            {
                std::printf(
                    "[config] '%s' loaded module '%s' from %s\n",
                    result.alias.c_str(), result.moduleName.c_str(), result.path.c_str());
            }
            else
            {
                std::fprintf(
                    stderr, "[config] '%s' failed: %s\n",
                    result.alias.c_str(), result.error.c_str());
            }
        }

        return true;
    }
}
