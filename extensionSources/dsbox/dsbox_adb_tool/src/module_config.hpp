#pragma once

#include <filesystem>

namespace dsbox
{
    class ModuleManager;
}

namespace dsbox_adb_tool
{
    // Loads every module declared in the config file at `path` (an
    // ad hoc module config passed on the command line, or
    // <base>/config/modules.ini). Returns false only if the file
    // itself couldn't be read; per-module problems are printed and
    // otherwise don't stop the rest of the file from loading.
    bool loadModuleConfig(
        dsbox::ModuleManager& modules,
        const std::filesystem::path& path,
        const std::filesystem::path& moduleBaseDir);
}
