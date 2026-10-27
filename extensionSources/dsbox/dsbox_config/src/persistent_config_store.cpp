#include <dsbox_config/persistent_config_store.hpp>

#include <dsbox_ini/ini_document.hpp>

#include <sstream>
#include <unordered_set>
#include <vector>

namespace dsbox_config
{
    namespace
    {
        std::string_view trim(std::string_view text)
        {
            std::size_t begin = text.find_first_not_of(" \t\r\n");

            if (begin == std::string_view::npos)
                return {};

            std::size_t end = text.find_last_not_of(" \t\r\n");

            return text.substr(begin, end - begin + 1);
        }

        std::string sectionKeyId(const std::string& section, const std::string& key)
        {
            return section + '\x1f' + key;
        }

        // Copies `originalText`, replacing only the lines whose value
        // actually changed between `originalDoc` and `currentDoc`.
        // Comments, blank lines, indentation, and anything else about
        // lines that didn't change are left byte-for-byte untouched.
        // Keys/sections present in currentDoc but not originalDoc are
        // appended at the end (new sections get a fresh header; new keys
        // in an existing section are appended after that section's
        // original lines).
        std::string patchOverridesText(
            const std::string& originalText,
            const dsbox_ini::IniDocument& originalDoc,
            const dsbox_ini::IniDocument& currentDoc)
        {
            std::vector<std::string> lines;
            {
                std::istringstream stream(originalText);
                std::string line;
                while (std::getline(stream, line))
                    lines.push_back(line);
            }

            std::string currentSection;
            std::unordered_set<std::string> emitted;

            for (auto& line : lines)
            {
                std::string_view trimmed = trim(line);

                if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#')
                    continue;

                if (trimmed.front() == '[')
                {
                    if (trimmed.back() == ']')
                        currentSection = std::string(trim(trimmed.substr(1, trimmed.size() - 2)));

                    continue;
                }

                std::size_t eq = trimmed.find('=');
                if (eq == std::string_view::npos)
                    continue;

                std::string key = std::string(trim(trimmed.substr(0, eq)));

                if (!currentDoc.hasKey(currentSection, key))
                    continue; // no longer known -- leave the line as-is

                emitted.insert(sectionKeyId(currentSection, key));

                if (originalDoc.hasKey(currentSection, key) &&
                    originalDoc.get(currentSection, key) == currentDoc.get(currentSection, key))
                    continue; // unchanged -- leave the line completely untouched

                std::size_t eqInLine = line.find('=');
                line = line.substr(0, eqInLine + 1) + " " + currentDoc.get(currentSection, key);
            }

            std::ostringstream appended;

            // IniDocument::sections() deliberately excludes the implicit
            // default/unnamed section (it has no header to print), but
            // that section can still gain brand new keys that must not
            // be silently dropped here -- so it's checked explicitly in
            // addition to every named section.
            std::vector<std::string> sectionsToCheck;
            sectionsToCheck.push_back(std::string());
            for (const auto& section : currentDoc.sections())
                sectionsToCheck.push_back(section);

            for (const auto& section : sectionsToCheck)
            {
                bool sectionExistsInOriginal = section.empty() || originalDoc.hasSection(section);
                std::vector<std::string> newKeys;

                for (const auto& key : currentDoc.keys(section))
                {
                    if (emitted.contains(sectionKeyId(section, key)))
                        continue;

                    if (originalDoc.hasKey(section, key))
                        continue;

                    newKeys.push_back(key);
                }

                if (newKeys.empty())
                    continue;

                if (!sectionExistsInOriginal)
                    appended << "\n[" << section << "]\n";

                for (const auto& key : newKeys)
                    appended << key << " = " << currentDoc.get(section, key) << "\n";
            }

            std::ostringstream result;
            for (const auto& line : lines)
                result << line << "\n";
            result << appended.str();

            return result.str();
        }
    }

    struct PersistentConfigStore::Impl
    {
        dsbox_ini::IniDocument defaults;
        dsbox_ini::IniDocument overrides;

        // Kept only to support ConfigSaveMode::PreserveFormatting.
        std::string originalOverridesText;
        dsbox_ini::IniDocument originalOverridesDoc;

        mutable std::shared_mutex mutex;
        bool dirty = false;
    };

    PersistentConfigStore::PersistentConfigStore()
        : m_impl(std::make_unique<Impl>())
    {
    }

    PersistentConfigStore::PersistentConfigStore(PersistentConfigStore&&) noexcept = default;
    PersistentConfigStore& PersistentConfigStore::operator=(PersistentConfigStore&&) noexcept = default;
    PersistentConfigStore::~PersistentConfigStore() = default;

    PersistentConfigStore PersistentConfigStore::load(
        std::string_view defaultsIniText,
        std::string_view overridesIniText)
    {
        PersistentConfigStore store;

        store.m_impl->defaults = dsbox_ini::IniDocument::parse(defaultsIniText);
        store.m_impl->overrides = dsbox_ini::IniDocument::parse(overridesIniText);

        store.m_impl->originalOverridesText = std::string(overridesIniText);
        store.m_impl->originalOverridesDoc = store.m_impl->overrides;

        return store;
    }

    std::string PersistentConfigStore::getOr(
        std::string_view section,
        std::string_view key,
        std::string defaultValue) const
    {
        std::shared_lock<std::shared_mutex> lock(m_impl->mutex);

        if (m_impl->overrides.hasKey(section, key))
            return m_impl->overrides.get(section, key);

        if (m_impl->defaults.hasKey(section, key))
            return m_impl->defaults.get(section, key);

        return defaultValue;
    }

    bool PersistentConfigStore::getBool(
        std::string_view section,
        std::string_view key,
        bool defaultValue) const
    {
        std::shared_lock<std::shared_mutex> lock(m_impl->mutex);

        if (m_impl->overrides.hasKey(section, key))
            return m_impl->overrides.getBool(section, key, defaultValue);

        if (m_impl->defaults.hasKey(section, key))
            return m_impl->defaults.getBool(section, key, defaultValue);

        return defaultValue;
    }

    void PersistentConfigStore::set(std::string_view section, std::string_view key, std::string value)
    {
        std::unique_lock<std::shared_mutex> lock(m_impl->mutex);

        m_impl->overrides.set(section, key, std::move(value));
        m_impl->dirty = true;
    }

    bool PersistentConfigStore::isDirty() const
    {
        std::shared_lock<std::shared_mutex> lock(m_impl->mutex);
        return m_impl->dirty;
    }

    std::string PersistentConfigStore::serializeOverrides(ConfigSaveMode mode) const
    {
        std::shared_lock<std::shared_mutex> lock(m_impl->mutex);

        if (mode == ConfigSaveMode::Regenerate)
            return m_impl->overrides.serialize();

        return patchOverridesText(
            m_impl->originalOverridesText, m_impl->originalOverridesDoc, m_impl->overrides);
    }

    void PersistentConfigStore::markSaved()
    {
        std::unique_lock<std::shared_mutex> lock(m_impl->mutex);
        m_impl->dirty = false;
    }
}
