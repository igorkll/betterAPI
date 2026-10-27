#include <dsbox_config/module_config_loader.hpp>

#include <dsbox/module.hpp>
#include <dsbox/module_manager.hpp>

#include <dsbox_ini/ini_document.hpp>

#include <cctype>

namespace dsbox_config
{
    namespace
    {
        constexpr std::string_view kModulesSection = "modules";
        constexpr std::string_view kPathSuffix = ".path";
        constexpr std::string_view kEnabledSuffix = ".enabled";

        bool endsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() &&
                text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool isAbsolutePath(std::string_view path)
        {
            if (path.empty())
                return false;

            if (path.front() == '/' || path.front() == '\\')
                return true;

            // Windows drive letter, e.g. "C:\..." or "C:/..."
            if (path.size() >= 2 &&
                std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
                return true;

            return false;
        }

        std::string resolvePath(std::string_view path, std::string_view baseDir)
        {
            if (baseDir.empty() || isAbsolutePath(path))
                return std::string(path);

            std::string result(baseDir);

            if (!result.empty() && result.back() != '/' && result.back() != '\\')
                result += '/';

            result += path;

            return result;
        }
    }

    ModuleLoadReport ModuleConfigLoader::loadFromIni(
        dsbox::ModuleManager& modules,
        std::string_view iniText,
        std::string_view baseDir)
    {
        ModuleLoadReport report;

        dsbox_ini::IniDocument doc;

        try
        {
            doc = dsbox_ini::IniDocument::parse(iniText);
        }
        catch (const dsbox_ini::IniParseError& error)
        {
            ModuleLoadResult result;
            result.alias = "<ini>";
            result.error = error.what();
            report.results.push_back(std::move(result));
            return report;
        }

        if (!doc.hasSection(kModulesSection))
            return report;

        std::vector<std::string> aliases;

        for (const auto& key : doc.keys(kModulesSection))
        {
            if (endsWith(key, kPathSuffix))
                aliases.push_back(key.substr(0, key.size() - kPathSuffix.size()));
        }

        for (const auto& alias : aliases)
        {
            ModuleLoadResult result;
            result.alias = alias;

            std::string rawPath = doc.getOr(kModulesSection, alias + std::string(kPathSuffix), "");
            result.path = resolvePath(rawPath, baseDir);

            try
            {
                result.enabled =
                    doc.getBool(kModulesSection, alias + std::string(kEnabledSuffix), true);
            }
            catch (const dsbox_ini::IniParseError& error)
            {
                result.error = error.what();
                report.results.push_back(std::move(result));
                continue;
            }

            if (!result.enabled)
            {
                report.results.push_back(std::move(result));
                continue;
            }

            try
            {
                dsbox::IModule& loaded = modules.loadDynamic(result.path);
                result.loaded = true;
                result.moduleName = std::string(loaded.name());
            }
            catch (const std::exception& error)
            {
                result.error = error.what();
            }

            report.results.push_back(std::move(result));
        }

        return report;
    }
}
