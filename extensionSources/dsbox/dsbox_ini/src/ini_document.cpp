#include <dsbox_ini/ini_document.hpp>

#include <cctype>
#include <sstream>

namespace dsbox_ini
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

        std::string toLower(std::string_view text)
        {
            std::string result;
            result.reserve(text.size());

            for (char c : text)
                result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            return result;
        }
    }

    IniDocument::KeyValues* IniDocument::findSection(std::string_view section)
    {
        for (auto& entry : m_sections)
            if (entry.first == section)
                return &entry.second;

        return nullptr;
    }

    const IniDocument::KeyValues* IniDocument::findSection(std::string_view section) const
    {
        for (const auto& entry : m_sections)
            if (entry.first == section)
                return &entry.second;

        return nullptr;
    }

    IniDocument IniDocument::parse(std::string_view text)
    {
        IniDocument doc;

        std::string currentSection; // "" = implicit default section
        doc.m_sections.push_back({ currentSection, {} });

        std::size_t pos = 0;
        int lineNumber = 0;

        while (pos <= text.size())
        {
            std::size_t newlinePos = text.find('\n', pos);

            std::string_view rawLine = (newlinePos == std::string_view::npos)
                ? text.substr(pos)
                : text.substr(pos, newlinePos - pos);

            ++lineNumber;

            std::string_view line = trim(rawLine);

            if (!line.empty() && line.front() != ';' && line.front() != '#')
            {
                if (line.front() == '[')
                {
                    if (line.back() != ']')
                        throw IniParseError(
                            "Malformed section header on line " + std::to_string(lineNumber));

                    currentSection = std::string(trim(line.substr(1, line.size() - 2)));

                    if (currentSection.empty())
                        throw IniParseError(
                            "Empty section name on line " + std::to_string(lineNumber));

                    if (doc.findSection(currentSection) != nullptr)
                        throw IniParseError(
                            "Section '" + currentSection + "' redefined on line " +
                            std::to_string(lineNumber));

                    doc.m_sections.push_back({ currentSection, {} });
                }
                else
                {
                    std::size_t eq = line.find('=');

                    if (eq == std::string_view::npos)
                        throw IniParseError(
                            "Malformed line " + std::to_string(lineNumber) +
                            " (expected 'key = value')");

                    std::string key = std::string(trim(line.substr(0, eq)));
                    std::string value = std::string(trim(line.substr(eq + 1)));

                    if (key.empty())
                        throw IniParseError("Empty key on line " + std::to_string(lineNumber));

                    KeyValues& kv = doc.m_sections.back().second;

                    for (const auto& existing : kv)
                    {
                        if (existing.first == key)
                            throw IniParseError(
                                "Duplicate key '" + key + "' in section '" + currentSection +
                                "' on line " + std::to_string(lineNumber));
                    }

                    kv.emplace_back(std::move(key), std::move(value));
                }
            }

            if (newlinePos == std::string_view::npos)
                break;

            pos = newlinePos + 1;
        }

        return doc;
    }

    bool IniDocument::hasSection(std::string_view section) const
    {
        return findSection(section) != nullptr;
    }

    bool IniDocument::hasKey(std::string_view section, std::string_view key) const
    {
        const KeyValues* kv = findSection(section);

        if (!kv)
            return false;

        for (const auto& entry : *kv)
            if (entry.first == key)
                return true;

        return false;
    }

    const std::string& IniDocument::get(std::string_view section, std::string_view key) const
    {
        const KeyValues* kv = findSection(section);

        if (kv)
            for (const auto& entry : *kv)
                if (entry.first == key)
                    return entry.second;

        throw IniParseError(
            "Missing key '" + std::string(key) + "' in section '" + std::string(section) + "'");
    }

    std::string IniDocument::getOr(
        std::string_view section,
        std::string_view key,
        std::string defaultValue) const
    {
        const KeyValues* kv = findSection(section);

        if (kv)
            for (const auto& entry : *kv)
                if (entry.first == key)
                    return entry.second;

        return defaultValue;
    }

    bool IniDocument::getBool(
        std::string_view section,
        std::string_view key,
        bool defaultValue) const
    {
        if (!hasKey(section, key))
            return defaultValue;

        std::string lower = toLower(get(section, key));

        if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
            return true;

        if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
            return false;

        throw IniParseError(
            "Invalid boolean value '" + get(section, key) + "' for key '" + std::string(key) +
            "' in section '" + std::string(section) + "'");
    }

    std::vector<std::string> IniDocument::keys(std::string_view section) const
    {
        std::vector<std::string> result;

        const KeyValues* kv = findSection(section);

        if (!kv)
            return result;

        result.reserve(kv->size());

        for (const auto& entry : *kv)
            result.push_back(entry.first);

        return result;
    }

    std::vector<std::string> IniDocument::sections() const
    {
        std::vector<std::string> result;
        result.reserve(m_sections.size());

        for (const auto& entry : m_sections)
            if (!entry.first.empty())
                result.push_back(entry.first);

        return result;
    }

    void IniDocument::set(std::string_view section, std::string_view key, std::string value)
    {
        KeyValues* kv = findSection(section);

        if (!kv)
        {
            m_sections.push_back({ std::string(section), {} });
            kv = &m_sections.back().second;
        }

        for (auto& entry : *kv)
        {
            if (entry.first == key)
            {
                entry.second = std::move(value);
                return;
            }
        }

        kv->emplace_back(std::string(key), std::move(value));
    }

    std::string IniDocument::serialize() const
    {
        std::ostringstream out;

        for (const auto& sectionEntry : m_sections)
        {
            const std::string& section = sectionEntry.first;
            const KeyValues& kv = sectionEntry.second;

            if (kv.empty())
                continue;

            if (!section.empty())
                out << "[" << section << "]\n";

            for (const auto& entry : kv)
                out << entry.first << " = " << entry.second << "\n";

            out << "\n";
        }

        std::string result = out.str();

        // The loop above adds one blank separator line after every
        // section, including the last -- trim that back down to a single
        // trailing newline so repeated serialize() calls don't visibly
        // grow trailing whitespace.
        while (!result.empty() && result.back() == '\n')
            result.pop_back();

        if (!result.empty())
            result += '\n';

        return result;
    }
}
